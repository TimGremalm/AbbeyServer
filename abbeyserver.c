#include "espressif/esp_common.h"
#include "esp/uart.h"

#include <string.h>

#include <FreeRTOS.h>
#include <task.h>
#include <ssid_config.h>

#include <espressif/esp_sta.h>
#include <espressif/esp_wifi.h>

#include <paho_mqtt_c/MQTTESP8266.h>
#include <paho_mqtt_c/MQTTClient.h>

#include <semphr.h>

#include "pwm.h"

#define MQTT_HOST ("192.168.0.186")
#define MQTT_PORT 1883

#define MQTT_USER NULL
#define MQTT_PASS NULL

SemaphoreHandle_t wifi_alive;
QueueHandle_t publish_queue;
#define PUB_MSG_LEN 16

#define SERVO_MIN 3277 // ((1000µs/20000µs)*UINT16_MAX)
#define SERVO_MID 4915 // ((1500µs/20000µs)*UINT16_MAX)
#define SERVO_MAX 6553 // ((2000µs/20000µs)*UINT16_MAX)

#define BELLS 6
TickType_t bellcalls[BELLS] = {0};
int bellCallHandeled[BELLS] = {1};

static void beat_task(void *pvParameters) {
	TickType_t xLastWakeTime = xTaskGetTickCount();
	char msg[PUB_MSG_LEN];
	int count = 0;

	while (1) {
		vTaskDelayUntil(&xLastWakeTime, 10000 / portTICK_PERIOD_MS);
		//printf("beat\n");
		snprintf(msg, PUB_MSG_LEN, "Beat %d\n", count++);
		if (xQueueSend(publish_queue, (void *)msg, 0) == pdFALSE) {
			printf("Publish queue overflow.\n");
		}
	}
}

static void topic_received(mqtt_message_data_t *md) {
	int i;
	mqtt_message_t *message = md->message;
	printf("Received: ");
	for( i = 0; i < md->topic->lenstring.len; ++i)
		printf("%c", md->topic->lenstring.data[ i ]);

	printf(" = ");
	for( i = 0; i < (int)message->payloadlen; ++i)
		printf("%c", ((char *)(message->payload))[i]);

	printf("\n");
	char buf[100] = {0};
	if (message->payloadlen < sizeof(buf)) {
		memcpy(buf, message->payload, message->payloadlen);
		int parsedNumber = atoi(buf);
		if (parsedNumber >= 0 && parsedNumber <= BELLS) {
			TickType_t now = xTaskGetTickCount();
			if (parsedNumber == 0) {
				printf("Call all bells at tick %d\n", now);
				for (i=0; i < BELLS; i++) {
					bellcalls[i] = now;
					bellCallHandeled[i] = 0;
				}
			} else {
				printf("Call bell: %d at tick %d\n", parsedNumber, now);
				bellcalls[parsedNumber - 1] = now;
				bellCallHandeled[parsedNumber - 1] = 0;
			}
		}
	}
}

static const char * get_my_id(void) {
	// Use MAC address for Station as unique ID
	static char my_id[13];
	static bool my_id_done = false;
	int8_t i;
	uint8_t x;
	if (my_id_done)
		return my_id;
	if (!sdk_wifi_get_macaddr(STATION_IF, (uint8_t *)my_id))
		return NULL;
	for (i = 5; i >= 0; --i) {
		x = my_id[i] & 0x0F;
		if (x > 9) x += 7;
		my_id[i * 2 + 1] = x + '0';
		x = my_id[i] >> 4;
		if (x > 9) x += 7;
		my_id[i * 2] = x + '0';
	}
	my_id[12] = '\0';
	my_id_done = true;
	return my_id;
}

static void mqtt_task(void *pvParameters) {
	int ret		 = 0;
	struct mqtt_network network;
	mqtt_client_t client   = mqtt_client_default;
	char mqtt_client_id[20];
	uint8_t mqtt_buf[100];
	uint8_t mqtt_readbuf[100];
	mqtt_packet_connect_data_t data = mqtt_packet_connect_data_initializer;

	mqtt_network_new( &network );
	memset(mqtt_client_id, 0, sizeof(mqtt_client_id));
	strcpy(mqtt_client_id, "ESP-");
	strcat(mqtt_client_id, get_my_id());

	while(1) {
		xSemaphoreTake(wifi_alive, portMAX_DELAY);
		printf("%s: started\n", __func__);
		printf("%s: (Re)connecting to MQTT server %s ... ",__func__,
			   MQTT_HOST);
		ret = mqtt_network_connect(&network, MQTT_HOST, MQTT_PORT);
		if(ret) {
			printf("error: %d\n", ret);
			taskYIELD();
			continue;
		}
		printf("done\n");
		mqtt_client_new(&client, &network, 5000, mqtt_buf, 100,
					  mqtt_readbuf, 100);

		data.willFlag	   = 0;
		data.MQTTVersion	= 3;
		data.clientID.cstring   = mqtt_client_id;
		data.username.cstring   = MQTT_USER;
		data.password.cstring   = MQTT_PASS;
		data.keepAliveInterval  = 10;
		data.cleansession   = 0;
		printf("Send MQTT connect ... ");
		ret = mqtt_connect(&client, &data);
		if(ret) {
			printf("error: %d\n", ret);
			mqtt_network_disconnect(&network);
			taskYIELD();
			continue;
		}
		printf("done\n");
		mqtt_subscribe(&client, "/bell", MQTT_QOS1, topic_received);
		xQueueReset(publish_queue);

		while(1) {
			char msg[PUB_MSG_LEN - 1] = "\0";
			while(xQueueReceive(publish_queue, (void *)msg, 0) == pdTRUE) {
				//printf("got message to publish\n");
				mqtt_message_t message;
				message.payload = msg;
				message.payloadlen = PUB_MSG_LEN;
				message.dup = 0;
				message.qos = MQTT_QOS1;
				message.retained = 0;
				ret = mqtt_publish(&client, "/beat", &message);
				if (ret != MQTT_SUCCESS ) {
					printf("error while publishing message: %d\n", ret );
					break;
				}
			}

			ret = mqtt_yield(&client, 1000);
			if (ret == MQTT_DISCONNECTED)
				break;
		}
		printf("Connection dropped, request restart\n");
		mqtt_network_disconnect(&network);
		taskYIELD();
	}
}

static void wifi_task(void *pvParameters) {
	uint8_t status  = 0;
	uint8_t retries = 30;
	struct sdk_station_config config = {
		.ssid = WIFI_SSID,
		.password = WIFI_PASS,
	};

	printf("WiFi: connecting to WiFi\n");
	sdk_wifi_set_opmode(STATION_MODE);
	sdk_wifi_station_set_config(&config);

	while(1) {
		while ((status != STATION_GOT_IP) && (retries)) {
			status = sdk_wifi_station_get_connect_status();
			printf("%s: status = %d\n", __func__, status );
			if( status == STATION_WRONG_PASSWORD ){
				printf("WiFi: wrong password\n");
				break;
			} else if( status == STATION_NO_AP_FOUND ) {
				printf("WiFi: AP not found\n");
				break;
			} else if( status == STATION_CONNECT_FAIL ) {
				printf("WiFi: connection failed\n");
				break;
			}
			vTaskDelay( 1000 / portTICK_PERIOD_MS );
			--retries;
		}
		if (status == STATION_GOT_IP) {
			printf("WiFi: Connected\n");
			xSemaphoreGive( wifi_alive );
			taskYIELD();
		}

		while ((status = sdk_wifi_station_get_connect_status()) == STATION_GOT_IP) {
			xSemaphoreGive( wifi_alive );
			taskYIELD();
		}
		printf("WiFi: disconnected\n");
		sdk_wifi_station_disconnect();
		vTaskDelay( 1000 / portTICK_PERIOD_MS );
	}
}

void servo_task(void *pvParameters) {
	uint8_t pins[] = {14, 12, 13, 15, 5, 4}; //NodeMCU D5-D10 https://github.com/nodemcu/nodemcu-devkit-v1.0#pin-map
	uint16_t servoRange = 2900;
	uint16_t servoStart = 3400;
	while(1) {
		for (int i=0; i < BELLS; i++) {
			TickType_t now = xTaskGetTickCount();
			uint16_t delta = now - bellcalls[i];
			float deltaFactor = delta * 130.0;
			uint16_t laps = deltaFactor / servoRange;
			int lapsMax = 6;
			if (laps < lapsMax && bellCallHandeled[i] == 0) {
				uint16_t rest = (int)deltaFactor % servoRange;
				uint16_t posOut = 0;
				//Make servos sweep from start to end, and back again
				if ((laps % 2) == 0) {
					posOut = servoStart + rest; //Add to start value at even laps
				} else {
					posOut = servoStart + servoRange - rest; //Subtract from end value at odd laps
				}
				//printf("Bell %d Lap: %d Servopos: %d\n", i+1, laps, posOut);
				pwm_init(1, &pins[i], false);
				pwm_set_freq(50);
				pwm_set_duty(posOut);
				pwm_start();
				vTaskDelay(1);
			} else {
				if (bellCallHandeled[i] == 0) {
					bellCallHandeled[i] = 1;
					printf("Bell stopped at %d\n", now);
				}
			}
		}
		pwm_stop();

		vTaskDelay(1);
	}
}

void user_init(void) {
	uart_set_baud(0, 115200);
	printf("SDK version:%s\n", sdk_system_get_sdk_version());

	vSemaphoreCreateBinary(wifi_alive);
	publish_queue = xQueueCreate(3, PUB_MSG_LEN);
	xTaskCreate(&wifi_task, "wifi_task",  256, NULL, 2, NULL);
	xTaskCreate(&beat_task, "beat_task", 256, NULL, 3, NULL);
	xTaskCreate(&mqtt_task, "mqtt_task", 1024, NULL, 4, NULL);
	xTaskCreate(&servo_task, "servo_task", 1024, NULL, 4, NULL);
}

