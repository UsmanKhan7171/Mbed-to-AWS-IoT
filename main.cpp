/*******************************************************************************
 * Copyright (c) 2014, 2015 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs - make sure QoS2 processing works, and add device headers
 *******************************************************************************/

 /**
  This is a sample program to illustrate the use of the MQTT Client library
  on the mbed platform.  The Client class requires two classes which mediate
  access to system interfaces for networking and timing.  As long as these two
  classes provide the required public programming interfaces, it does not matter
  what facilities they use underneath. In this program, they use the mbed
  system libraries.

 */

#define MQTTCLIENT_QOS1 0
#define MQTTCLIENT_QOS2 0

#include "mbed.h"
#include "UbloxATCellularInterface.h"
#include "UbloxATCellularInterfaceN2xx.h"
#include "NTPClient.h"
#include "MQTTNetwork.h"
#include "MQTTmbed.h"
#include "MQTTClient.h"
#include "MQTT_server_setting.h"
#include "mbed-trace/mbed_trace.h"
#include "mbed_events.h"
#include "mbedtls/error.h"
#include "humidity-temperature-sensor-si7034-a10/Si7034.h"

#define PIN_I2C_SDA  PC_9
#define PIN_I2C_SCL  PA_8

#define INTERFACE_CLASS  UbloxATCellularInterface
#define PIN "0000"
#define APN         NULL
#define USERNAME    NULL
#define PASSWORD    NULL
#define LED_ON  MBED_CONF_APP_LED_ON
#define LED_OFF MBED_CONF_APP_LED_OFF
#define PUBLISH_TEMP   112
#define PUBLISH_LAUNCH 0

static volatile bool isPublish = false;

/* Flag to be set when received a message from the server. */
static volatile bool isMessageArrived = false;
/* Buffer size for a receiving message. */
const int MESSAGE_BUFFER_SIZE = 256;
/* Buffer for a receiving message. */
char messageBuffer[MESSAGE_BUFFER_SIZE];

// An event queue is a very useful structure to debounce information between contexts (e.g. ISR and normal threads)
// This is great because things such as network operations are illegal in ISR, so updating a resource in a button's fall() function is not allowed
EventQueue eventQueue;
Thread thread1;

/*
 * Callback function called when a message arrived from server.
 */
void messageArrived(MQTT::MessageData& md)
{
    // Copy payload to the buffer.
    MQTT::Message &message = md.message;
    if(message.payloadlen >= MESSAGE_BUFFER_SIZE) {
        // TODO: handling error
    } else {
        memcpy(messageBuffer, message.payload, message.payloadlen);
    }
    messageBuffer[message.payloadlen] = '\0';

    isMessageArrived = true;
}

/*
 * Callback function called when the button1 is clicked.
 */
void btn1_rise_handler() {
    isPublish = true;
}


int main(int argc)
{
	printf("Starting Application\r\n");
    mbed_trace_init();

    INTERFACE_CLASS *interface = new INTERFACE_CLASS();
    NetworkInterface* network = interface;
    MQTTNetwork* mqttNetwork = NULL;
    MQTT::Client<MQTTNetwork, Countdown>* mqttClient = NULL;
    DigitalOut led(MBED_CONF_APP_LED_PIN, LED_ON);

    const float version = 0.9;
    bool isSubscribed = false;
	char  revv[2];
	char e_id[9];
	float tempH;
	float tempC;
	int rc_publish;
	static unsigned short id = 0;

	MQTT::Message message;
//	MQTT::Message message_hr;

	char * hr_payload;
	int len;

	DigitalOut gpio_enable_si(PD_14);
	gpio_enable_si=1;

    Si7034 si1(PIN_I2C_SDA, PIN_I2C_SCL);

	si1.reset();
	wait(1);

	if (!interface) {
		printf("Error! No network inteface found.\n");
		return -1;
	}

	// Check all of the IMEI, MEID, IMSI and ICCID calls
	const char *imei = interface->imei();
	if (imei != NULL) {
		printf("IMEI is %s \r\n", imei);
	} else {
		printf("IMEI not found\r\n");
	}

	interface->set_credentials(APN, USERNAME, PASSWORD);
	printf("Connecting to network\n");
	int x;
	for (x = 0; interface->connect(PIN) != 0; x++) {
		if (x > 0) {
			printf("Retrying (have you checked that an antenna is plugged in and your APN is correct?)...\n");
		}
	}
    printf("Network interface opened successfully.\r\n");

    // sync the real time clock (RTC)
    NTPClient ntp(interface);
    ntp.set_server("0.uk.pool.ntp.org", 123);
    time_t now = 0;
    char buffer[32];

    now = ntp.get_timestamp();

    if(now > 0 && now != 536898160){
    	set_time(now);
		printf("Current Time: %s", ctime(&now));
    }else{
        printf("Disconnecting Client.\r\n");
        {
			if(mqttClient) {
				if(isSubscribed) {
					mqttClient->unsubscribe(MQTT_TOPIC_SUB);
					mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
				}
				if(mqttClient->isConnected())
					mqttClient->disconnect();
				delete mqttClient;
			}
			if(mqttNetwork) {
				mqttNetwork->disconnect();
				delete mqttNetwork;
			}
			if(interface) {
				interface->disconnect();
				// network is not created by new.
			}
        }
    	NVIC_SystemReset();
    }

    printf("Connecting to host %s:%d ...\r\n", MQTT_SERVER_HOST_NAME, MQTT_SERVER_PORT);
    {
        mqttNetwork = new MQTTNetwork(interface);
        int rc = mqttNetwork->connect(MQTT_SERVER_HOST_NAME, MQTT_SERVER_PORT, SSL_CA_PEM,
                SSL_CLIENT_CERT_PEM, SSL_CLIENT_PRIVATE_KEY_PEM);
        if (rc != MQTT::SUCCESS){
            const int MAX_TLS_ERROR_CODE = -0x1000;
            // Network error
            if((MAX_TLS_ERROR_CODE < rc) && (rc < 0)) {
                // TODO: implement converting an error code into message.
                printf("ERROR from MQTTNetwork connect is %d. \r\n", rc);
            }
            // TLS error - mbedTLS error codes starts from -0x1000 to -0x8000.
            if(rc <= MAX_TLS_ERROR_CODE) {
                const int buf_size = 256;
                char *buf = new char[buf_size];
                mbedtls_strerror(rc, buf, buf_size);
                printf("TLS ERROR (%d) : %s\r\n", rc, buf);
                delete[] buf;
            }
            printf("ERROR in MQTTNetwork connect \r\n");
            NVIC_SystemReset();
        }
    }
    printf("Connection established.\r\n");
    printf("\r\n");

    printf("MQTT client is trying to connect the server ...\r\n");
    {
        MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
        data.MQTTVersion = 3;
        data.clientID.cstring = (char *)imei;
        data.username.cstring = (char *)MQTT_USERNAME;
        data.password.cstring = (char *)MQTT_PASSWORD;
        printf("Client ID %s\r\n", data.clientID.cstring);

        mqttClient = new MQTT::Client<MQTTNetwork, Countdown>(*mqttNetwork);
        int rc = mqttClient->connect(data);
        if (rc != MQTT::SUCCESS) {
            printf("ERROR: rc from MQTT connect is %d\r\n", rc);
            printf("Restarting Client.\r\n");
            {
				if(mqttClient) {
					if(isSubscribed) {
						mqttClient->unsubscribe(MQTT_TOPIC_SUB);
						mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
					}
					if(mqttClient->isConnected())
						mqttClient->disconnect();
					delete mqttClient;
				}
				if(mqttNetwork) {
					mqttNetwork->disconnect();
					delete mqttNetwork;
				}
				if(interface) {
					interface->disconnect();
					// network is not created by new.
				}
            }
            NVIC_SystemReset();
        }
    }
    printf("Client connected.\r\n");
    printf("\r\n");


    printf("Client is trying to subscribe a topic \"%s\".\r\n", MQTT_TOPIC_SUB);
    {
        int rc = mqttClient->subscribe(MQTT_TOPIC_SUB, MQTT::QOS0, messageArrived);
        if (rc != MQTT::SUCCESS) {
            printf("ERROR: rc from MQTT subscribe is %d\r\n", rc);
            printf("Disconnecting Client.\r\n");
            {
				if(mqttClient) {
					if(isSubscribed) {
						mqttClient->unsubscribe(MQTT_TOPIC_SUB);
						mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
					}
					if(mqttClient->isConnected())
						mqttClient->disconnect();
					delete mqttClient;
				}
				if(mqttNetwork) {
					mqttNetwork->disconnect();
					delete mqttNetwork;
				}
				if(interface) {
					interface->disconnect();
					// network is not created by new.
				}
            }
            NVIC_SystemReset();
        }
        isSubscribed = true;
    }
    printf("Client has subscribed a topic \"%s\".\r\n", MQTT_TOPIC_SUB);
    printf("\r\n");

    char * time_buff, *src, *dst;
    const size_t buf_size = 200;
    while(1) {
    	si1.getTemperature(&tempC);
    	printf("Temp in Celcius %f \r\n", tempC);

        time_t seconds = time(NULL);
        time_buff = ctime(&seconds);
        printf("Current Time:  %s", time_buff);
        wait(1);

        // Relevant code in this section.
         src = time_buff;
         for (src = dst = time_buff; *src != '\0'; src++) {
             *dst = *src;
             if(*dst != ' ')
             {
            	 if (*dst != '\n') dst++;
             }
             else if(*dst == ' ')
             {
            	*dst = '-';
            	dst++;
             }
         }
         *dst = '\0';

    	/* Check connection */
        if(!mqttClient->isConnected()){
            printf("Disconnecting Client.\r\n");
            {
				if(mqttClient) {
					if(isSubscribed) {
						mqttClient->unsubscribe(MQTT_TOPIC_SUB);
						mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
					}
					if(mqttClient->isConnected())
						mqttClient->disconnect();
					delete mqttClient;
				}
				if(mqttNetwork) {
					mqttNetwork->disconnect();
					delete mqttNetwork;
				}
				if(interface) {
					interface->disconnect();
					// network is not created by new.
				}
            }
        	NVIC_SystemReset();
        }
        /* Pass control to other thread. */
        if(mqttClient->yield() != MQTT::SUCCESS) {
            printf("Disconnecting Client.\r\n");
            {
				if(mqttClient) {
					if(isSubscribed) {
						mqttClient->unsubscribe(MQTT_TOPIC_SUB);
						mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
					}
					if(mqttClient->isConnected())
						mqttClient->disconnect();
					delete mqttClient;
				}
				if(mqttNetwork) {
					mqttNetwork->disconnect();
					delete mqttNetwork;
				}
				if(interface) {
					interface->disconnect();
					// network is not created by new.
				}
            }
        	NVIC_SystemReset();
        }
        /* Received a control message. */
        if(isMessageArrived) {
            isMessageArrived = false;
            // Just print it out here.
            printf("\r\nMessage arrived:\r\n%s\r\n\r\n", messageBuffer);
        }
        /* Publish data */
        {
            isPublish = false;
            message.retained = false;
            message.dup = false;

            char *buf = new char[buf_size];
            len=sprintf(buf,
					"{\"Temp\":\"%.1f\",\"Time\":\"%s\",\"IMEI\":\"%s\"}",
					tempC,time_buff,imei
					);

			if(len < 0) {
				printf("ERROR: sprintf() returns %d \r\n", len);
				continue;
			}

            message.payload = (void*)buf;
            message.qos = MQTT::QOS0;
            printf("Packet ID %d \r\n",id);
            message.id = id++;
            message.payloadlen = len;

			// Publish a message.
            if(id == PUBLISH_LAUNCH){
            	rc_publish = mqttClient->publish(MQTT_TOPIC_SUB, message);
            	led = LED_ON;             // When sending a message, LED lights blue.
				wait(1);
				led = LED_OFF;
            }else if(id > PUBLISH_TEMP){
            	rc_publish = mqttClient->publish(MQTT_TOPIC_SUB, message);
            	led = LED_ON;
            	wait(1);
            	led = LED_OFF;
            	id = 1;
            }else{
            	message.payloadlen = sprintf(hr_payload,"{\"HearBeat\":\"%s\",\"PacketId\":\"%d\"}",time_buff,message.id);
            	message.payload = (void*)hr_payload;
            	rc_publish = mqttClient->publish(MQTT_TOPIC_HR, message);
            	led = LED_ON;
            	wait(1);
            	led = LED_OFF;
            }
            if(rc_publish != MQTT::SUCCESS) {
                printf("ERROR: rc from MQTT publish is %d\r\n", rc_publish);
                printf("Disconnecting Client.\r\n");
                {
					if(mqttClient) {
						if(isSubscribed) {
							mqttClient->unsubscribe(MQTT_TOPIC_SUB);
							mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
						}
						if(mqttClient->isConnected())
							mqttClient->disconnect();
						delete mqttClient;
					}
					if(mqttNetwork) {
						mqttNetwork->disconnect();
						delete mqttNetwork;
					}
					if(interface) {
						interface->disconnect();
						// network is not created by new.
					}
                }
                NVIC_SystemReset();
            }
            delete[] buf;    

            wait(30);
        }
    }

    printf("The client has disconnected.\r\n");

    if(mqttClient) {
        if(isSubscribed) {
            mqttClient->unsubscribe(MQTT_TOPIC_SUB);
            mqttClient->setMessageHandler(MQTT_TOPIC_SUB, 0);
        }
        if(mqttClient->isConnected()) 
            mqttClient->disconnect();
        delete mqttClient;
    }
    if(mqttNetwork) {
        mqttNetwork->disconnect();
        delete mqttNetwork;
    }
    if(interface) {
    	interface->disconnect();
        // network is not created by new.
    }
}
