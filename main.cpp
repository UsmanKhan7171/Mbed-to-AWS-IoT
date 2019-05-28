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
#include "battery-gauge-bq27441/battery_gauge_bq27441.h"

#define MQTTCLIENT_QOS1 0
#define MQTTCLIENT_QOS2 0
#define PIN_I2C_SDA  PC_9
#define PIN_I2C_SCL  PA_8
#define INTERFACE_CLASS  UbloxATCellularInterface
#define PIN "0000"
#define APN         NULL
#define USERNAME    NULL
#define PASSWORD    NULL
#define LED_ON  MBED_CONF_APP_LED_ON
#define LED_OFF MBED_CONF_APP_LED_OFF
#define PUBLISH_TEMP  51
#define PUBLISH_HUM   PUBLISH_TEMP+1
#define WAIT_INTERVAL  70
#define PUBLISH_LAUNCH 0
#define DEFAULT_RTC_TIME 536898160
#define TIME_ZONE_OFFSET 0 // 18000 for Lahore, it has to be 0 for UK

static volatile bool isPublish = false;
/* Flag to be set when received a message from the server. */
static volatile bool isMessageArrived = false;
/* Buffer size for a receiving message. */
const int MESSAGE_BUFFER_SIZE = 256;
/* Buffer for a receiving message. */
char messageBuffer[MESSAGE_BUFFER_SIZE];

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

void client_reset(MQTT::Client<MQTTNetwork, Countdown>* mqttClient,bool isSubscribed, MQTTNetwork* mqttNetwork,INTERFACE_CLASS *interface){
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
	NVIC_SystemReset();
}

void publish_packet(MQTT::Message &message, char * buf , unsigned short & id , int &rc_publish , MQTT::Client<MQTTNetwork, Countdown>* mqttClient, int &len)
{
	printf("Packet ID %d \r\n",id);
	message.payload = (void*)buf;
	message.qos = MQTT::QOS0;
	message.id = id;
	message.payloadlen = len;

	// Publish a message.
	if(id == PUBLISH_LAUNCH){
		rc_publish = mqttClient->publish(MQTT_TOPIC_SUB, message);
		id++;
	}else if(id == PUBLISH_TEMP){
		rc_publish = mqttClient->publish(MQTT_TOPIC_SUB, message);
		id++;
	}else if(id == PUBLISH_HUM){
		rc_publish = mqttClient->publish(MQTT_TOPIC_SUB, message);
		id = 1;
	}else{
		rc_publish = mqttClient->publish(MQTT_TOPIC_HR, message);
		id++;
	}
}

int main()
{
	printf("Starting Application\r\n");
    mbed_trace_init();

    INTERFACE_CLASS *interface = new INTERFACE_CLASS();
    MQTTNetwork* mqttNetwork = NULL;
    MQTT::Client<MQTTNetwork, Countdown>* mqttClient = NULL;
    DigitalOut led(MBED_CONF_APP_LED_PIN, LED_ON);
    I2C i2C(I2C_SDA_B, I2C_SCL_B);
	BatteryGaugeBq27441 gauge;
	int32_t battery_pctg = 0;

    bool isSubscribed = false;
	int rc_publish;
	float tempC;
	float tempH;
	unsigned short id = 0;
	int len=0;
    char * time_buff, *src, *dst;

	MQTT::Message message;
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
    now = ntp.get_timestamp();

    if(now > 0 && now != DEFAULT_RTC_TIME){
    	set_time(now);
		printf("Current Time: %s\r\n", ctime(&now));
    }else{
    	client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
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
			client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
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
			client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
        }
        isSubscribed = true;
    }
    printf("Client has subscribed a topic \"%s\".\r\n", MQTT_TOPIC_SUB);
    printf("\r\n");

    const size_t buf_size = 200;

    while(1) {
        time_t seconds = time(NULL) + TIME_ZONE_OFFSET;
        time_buff = ctime(&seconds);
        printf("Current Time:  %s\r\n", time_buff);
        wait(1);
		src = time_buff;
		for (src = dst = time_buff; *src != '\0'; src++) {
		    *dst = *src;
		    if(*dst != ' '){
			   if (*dst != '\n') dst++;
		    } else if(*dst == ' '){
			   *dst = '-';
			   dst++;
		    }
		}
		*dst = '\0';

		if (gauge.init(&i2C)){
			if (gauge.getRemainingPercentage(&battery_pctg))
				printf("Remaining battery percentage: %d%%.\r\n", (int) battery_pctg);
		}
    	/* Check connection */
        if(!mqttClient->isConnected()){
            printf("Disconnecting Client.\r\n");
			client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
        }
        /* Pass control to other thread. */
        if(mqttClient->yield() != MQTT::SUCCESS) {
            printf("Disconnecting Client.\r\n");
			client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
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

		if(id != PUBLISH_LAUNCH && id < PUBLISH_TEMP){
			len = sprintf(buf,"{\"HeartBeat\":\"%s\",\"PacketId\":\"%d\",\"Battery %\":\"%d\"}",time_buff,message.id,battery_pctg);
			if(len < 0) {
				printf("ERROR: sprintf() returns %d \r\n", len);
				continue;
			}
			publish_packet(message, buf , id , rc_publish , mqttClient , len); // Publish HeartBeat
		}else{
			si1.getTemperature(&tempC);
			printf("Temp in Celcius %f \r\n", tempC);
			len=sprintf(buf,
					"{\"Temp\":\"%.1f\",\"D/T\":\"%s\",\"IMEI\":\"%s\"}",
					tempC,time_buff,imei
					);
			if(len < 0) {
				printf("ERROR: sprintf() returns %d \r\n", len);
				continue;
			}
			publish_packet(message, buf , id , rc_publish , mqttClient , len); // Publish Temperature

			si1.getHumidity(&tempH);
			printf(" Humidity in RH is %f \r\n", tempH);
			len=sprintf(buf,
					"{\"Humd\":\"%.1f\",\"D/T\":\"%s\",\"IMEI\":\"%s\"}",
					tempH,time_buff,imei
					);
			if(len < 0) {
				printf("ERROR: sprintf() returns %d \r\n", len);
				continue;
			}
			publish_packet(message, buf , id , rc_publish , mqttClient , len); // Publish Humidity
		}
		led = LED_ON;
		wait(1);
		led = LED_OFF;

		if(rc_publish != MQTT::SUCCESS) {
			printf("ERROR: rc from MQTT publish is %d\r\n", rc_publish);
			printf("Disconnecting Client.\r\n");
			client_reset(mqttClient,isSubscribed,mqttNetwork,interface);
		}
		delete[] buf;

		wait(WAIT_INTERVAL);
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
