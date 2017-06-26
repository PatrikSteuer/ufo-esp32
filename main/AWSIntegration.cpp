#include "Ufo.h"
#include "aws_iot_config.h"
#include <aws_iot_log.h>
#include <aws_iot_version.h>
#include <aws_iot_mqtt_client_interface.h>
#include "Config.h"
#include "String.h"
#include <esp_log.h>
#include <cJSON.h>
#include "AWSIntegration.h"
#include "certs.h"

static const char* LOGTAG = "AWS";

void task_function_aws(void *pvParameter)
{
	((AWSIntegration*)pvParameter)->Connect();
	vTaskDelete(NULL);
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
									IoT_Publish_Message_Params *params, void *pData) {
	IOT_UNUSED(pData);
	IOT_UNUSED(pClient);
	ESP_LOGI(LOGTAG, "Message received");
	ESP_LOGI(LOGTAG, "%s: %s", topicName, (char*)params->payload);
}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
	ESP_LOGW(LOGTAG, "MQTT Disconnect");
	IoT_Error_t rc = FAILURE;

	if(NULL == pClient) {
		return;
	}

	IOT_UNUSED(data);

	if(aws_iot_is_autoreconnect_enabled(pClient)) {
		ESP_LOGI(LOGTAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
	} else {
		ESP_LOGW(LOGTAG, "Auto Reconnect not enabled. Starting manual reconnect...");
		rc = aws_iot_mqtt_attempt_reconnect(pClient);
		if(NETWORK_RECONNECTED == rc) {
			ESP_LOGW(LOGTAG, "Manual Reconnect Successful");
		} else {
			ESP_LOGW(LOGTAG, "Manual Reconnect Failed - %d", rc);
		}
	}
}

AWSIntegration::AWSIntegration() {
	ESP_LOGI(LOGTAG, "Start");
}

AWSIntegration::~AWSIntegration() {

}

bool AWSIntegration::Init(Ufo* pUfo) {
    mpUfo = pUfo;  
	xTaskCreate(&task_function_aws, "Task_AWS", 8192, this, 5, NULL); 	
	return true;
}

bool AWSIntegration::Connect() {

	vTaskDelay(12000 / portTICK_PERIOD_MS);
	ESP_LOGI(LOGTAG, "Connecting");

    while (!mInitialized) {
        if (mpUfo->GetWifi().IsConnected()) {
			ESP_LOGI(LOGTAG, "Init");
			char HostAddress[255] = AWS_IOT_MQTT_HOST;	
			mqttInitParams = iotClientInitParamsDefault;

			mqttInitParams.enableAutoReconnect = false; // We enable this later below
			mqttInitParams.pHostURL = HostAddress;
			mqttInitParams.port = AWS_IOT_MQTT_PORT;
			mqttInitParams.pRootCALocation = rootCA;
			mqttInitParams.pDeviceCertLocation = deviceCert;
			mqttInitParams.pDevicePrivateKeyLocation = devicePrivateKey;
			mqttInitParams.mqttCommandTimeout_ms = 20000;
			mqttInitParams.tlsHandshakeTimeout_ms = 5000;
			mqttInitParams.isSSLHostnameVerify = true;
			mqttInitParams.disconnectHandler = disconnectCallbackHandler;
			mqttInitParams.disconnectHandlerData = NULL;

			IoT_Error_t rc = aws_iot_mqtt_init(&client, &mqttInitParams);
			if (rc) {
				ESP_LOGE(LOGTAG, "AWS Init Error: %i", rc);
				return false;
			}
			mInitialized = true;
		}
		vTaskDelay(1);
	}

	connectParams = iotClientConnectParamsDefault;    
	ESP_LOGI(LOGTAG, "Connecting to %s", AWS_IOT_MQTT_HOST);
    IoT_Error_t rc = aws_iot_mqtt_connect(&client, &connectParams);
    if (rc) {
        ESP_LOGE(LOGTAG, "AWS Connect Error: %i", rc);
        return false;
    }
	ESP_LOGI(LOGTAG, "connected successfully");
	mActive = true;
    return Run();
}

bool AWSIntegration::Run() {
	IoT_Error_t rc;
	
	rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
	if(SUCCESS != rc) {
		ESP_LOGE(LOGTAG, "Unable to set Auto Reconnect to true - %d", rc);
		return false;
	}

	String topic("/dynatraceufo/");
	topic.printf("%s", AWS_IOT_MQTT_CLIENT_ID);
	// topic.printf(AWS_IOT_MQTT_CLIENT_ID);
	ESP_LOGI(LOGTAG, "Subscribing to %s", topic.c_str());
	rc = aws_iot_mqtt_subscribe(&client, topic.c_str(), topic.length(), QOS1, iot_subscribe_callback_handler, NULL);
	if(SUCCESS != rc) {
		ESP_LOGE(LOGTAG, "Error subscribing : %d ", rc);
		return false;
	}
	ESP_LOGI(LOGTAG, "subscription successful");

	while (mActive) {
		String topic("/dynatraceufo/ufohub");
		String payload("message from ");
		payload.printf("%s", AWS_IOT_MQTT_CLIENT_ID);
		Publish(topic, payload);
		vTaskDelay(10000 / portTICK_PERIOD_MS);
	}

	return false;
}

bool AWSIntegration::Publish(String pTopic, String pPayload) {

	IoT_Publish_Message_Params params;
	IoT_Error_t rc = FAILURE;
	
	params.qos = QOS0;
	params.payload = (void *) pPayload.c_str();
	params.isRetained = 0;

	//Max time the yield function will wait for read messages
	rc = aws_iot_mqtt_yield(&client, 100);
	while (NETWORK_ATTEMPTING_RECONNECT == rc) {
		rc = aws_iot_mqtt_yield(&client, 100);
	}

	params.payloadLen = pPayload.length();
	rc = aws_iot_mqtt_publish(&client, pTopic.c_str(), pTopic.length(), &params);

	if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
		ESP_LOGW(LOGTAG, "QOS1 publish ack not received.");
		rc = SUCCESS;
	}

	if(SUCCESS != rc) {
		ESP_LOGE(LOGTAG, "An error occurred in Publish: %i", rc);
	} else {
		ESP_LOGI(LOGTAG, "successfully published to %s", pTopic.c_str());
	}

	return rc;	

}
