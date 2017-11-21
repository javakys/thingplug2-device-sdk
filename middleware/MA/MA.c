/**
 * @file MA.c
 *
 * @brief MangementAgent
 *
 * Copyright (C) 2017. SK Telecom, All Rights Reserved.
 * Written 2017, by SK Telecom
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "MA.h"
#include "SRA.h"
#include "SMA.h"

#include "ThingPlug.h"
#include "Simple.h"

#include "Configuration.h"
#include "SKTtpDebug.h"

#define MQTT_CLIENT_ID                      "%s_%s"
#define MQTT_TOPIC_CONTROL_DOWN             "v1/dev/%s/%s/down"

#define TOPIC_SUBSCRIBE_SIZE                1

#define SIZE_RESPONSE_CODE                  10
#define SIZE_RESPONSE_MESSAGE               128
#define SIZE_TOPIC                          128
#define SIZE_PAYLOAD                        2048
#define SIZE_CLIENT_ID                      24

static enum PROCESS_STEP
{
    PROCESS_START = 0,
    PROCESS_ATTRIBUTE,
    PROCESS_TELEMETRY,
    PROCESS_END
} mStep;

static enum CONNECTION_STATUS
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED
} mConnectionStatus;

typedef struct
{
    /** device IP address **/
    char deviceIpAddress[30];
    /** gateway IP address **/
    char gatewayIpAddress[30];
} NetworkInfo;

static char mTopicControlDown[SIZE_TOPIC] = "";
static char mClientID[SIZE_CLIENT_ID] = "";

static void attribute();
static void telemetry();

void MQTTConnected(int result) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTConnected result : %d", result);
    // if connection failed
    if(result) {
        mConnectionStatus = DISCONNECTED;
    } else {
        mConnectionStatus = CONNECTED;
    }
    SKTDebugPrint(LOG_LEVEL_INFO, "CONNECTION_STATUS : %d", mConnectionStatus);
}

void MQTTSubscribed(int result) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTSubscribed result : %d", result);
    attribute();
}

void MQTTDisconnected(int result) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTDisconnected result : %d", result);
}

void MQTTConnectionLost(char* cause) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTConnectionLost result : %s", cause);
    mConnectionStatus = DISCONNECTED;
}

void MQTTMessageDelivered(int token) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTMessageDelivered token : %d, step : %d", token, mStep);
}

void MQTTMessageArrived(char* topic, char* msg, int msgLen) {
    SKTDebugPrint(LOG_LEVEL_INFO, "MQTTMessageArrived topic : %s, step : %d", topic, mStep);

	if(msg == NULL || msgLen < 1) {
		return;
    }
    char payload[SIZE_PAYLOAD] = "";
    memcpy(payload, msg, msgLen);
    SKTDebugPrint(LOG_LEVEL_INFO, "payload : %s", payload);
    
    cJSON* root = cJSON_Parse(payload);
    if(!root) return;

    cJSON* rpcReqObject = cJSON_GetObjectItemCaseSensitive(root, "rpcReq");
    // if RPC control
    if(rpcReqObject) {
        cJSON* rpcObject = cJSON_GetObjectItemCaseSensitive(rpcReqObject, "jsonrpc");
        cJSON* idObject = cJSON_GetObjectItemCaseSensitive(rpcReqObject, "id");
        cJSON* paramsObject = cJSON_GetObjectItemCaseSensitive(rpcReqObject, "params");
        cJSON* methodObject = cJSON_GetObjectItemCaseSensitive(rpcReqObject, "method");
        if(!idObject || !paramsObject || !methodObject) return;
        char* rpc = rpcObject->valuestring;        
        int id = idObject->valueint;
        char* method = methodObject->valuestring;
        cJSON* paramObject = cJSON_GetArrayItem(paramsObject, 0);
        cJSON* cmdObject = cJSON_GetObjectItemCaseSensitive(paramObject, "act7colorLed");
        if(!cmdObject) return;
        int cmd = cmdObject->valueint;
        SKTDebugPrint(LOG_LEVEL_INFO, "\nrpc : %s,\nid : %d,\ncmd : %d", rpc, id, cmd);
        int rc = RGB_LEDControl(cmd);
        RPCResponse rsp;
        memset(&rsp, 0, sizeof(RPCResponse));
        rsp.cmd = "rpc_json";
        rsp.cmdId = 1;
        rsp.jsonrpc = rpc;
        rsp.id = id;
        rsp.method = method;
        if(rc == 0) {
            rsp.result = "SUCCESS";
        } else {
            rsp.errorCode = 106;
            rsp.errorMessage = "FAIL";
        }
        tpSimpleResult(&rsp);
    } else {
        cJSON* cmdObject = cJSON_GetObjectItemCaseSensitive(root, "cmd");
        cJSON* cmdIdObject = cJSON_GetObjectItemCaseSensitive(root, "cmdId");
        if(!cmdObject || !cmdIdObject) return;
        char* cmd = cmdObject->valuestring;
        int cmdId = cmdIdObject->valueint;
        if(!cmd) return;
        // if attribute control
        if(strncmp(cmd, "set_attr", strlen("set_attr")) == 0) {
            cJSON* attribute = cJSON_GetObjectItemCaseSensitive(root, "attribute");
            if(!attribute) return;
            cJSON* act7colorLedObject = cJSON_GetObjectItemCaseSensitive(attribute, "act7colorLed");
            if(!act7colorLedObject) return;
            int act7colorLed = act7colorLedObject->valueint;
            SKTDebugPrint(LOG_LEVEL_INFO, "act7colorLed : %d, %d", act7colorLed, cmdId);
            int rc = RGB_LEDControl(act7colorLed);

            ArrayElement* arrayElement = calloc(1, sizeof(ArrayElement));
            arrayElement->capacity = 1;
            arrayElement->element = calloc(1, sizeof(Element) * arrayElement->capacity);
            Element* item = arrayElement->element + arrayElement->total;
            item->type = JSON_TYPE_LONG;
            item->name = "act7colorLed";
            if(rc != 0) {
                act7colorLed = RGB_LEDStatus();
            }
            item->value = &act7colorLed;
            arrayElement->total++;
            tpSimpleAttribute(arrayElement);
            free(arrayElement->element);
            free(arrayElement);
        }
    }
    cJSON_Delete(root);
}

long long current_timestamp() {
    struct timeval te;
    gettimeofday(&te, NULL); // get current time
    // long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000; // caculate milliseconds
    // return milliseconds;
    return te.tv_sec;
}

char *sensor_list[] = { "temp1", "humi1", "light1" };

static void telemetry() {
    mStep = PROCESS_TELEMETRY;
    // TODO make data
    // int i;
    char *temp, *humi, *light;
    int len;
    ArrayElement* arrayElement = calloc(1, sizeof(ArrayElement));
    
    arrayElement->capacity = 4;
    arrayElement->element = calloc(1, sizeof(Element) * arrayElement->capacity);
    
    SMAGetData(sensor_list[arrayElement->total], &temp, &len);
    temp = SRAConvertRawData(temp);
    Element* item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_RAW;
    item->name = sensor_list[arrayElement->total];
    item->value = temp;
    arrayElement->total++;

    SMAGetData(sensor_list[arrayElement->total], &humi, &len);
    humi = SRAConvertRawData(humi);
    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_RAW;
    item->name = sensor_list[arrayElement->total];
    item->value = humi;
    arrayElement->total++;
    
    SMAGetData(sensor_list[arrayElement->total], &light, &len);
    light = SRAConvertRawData(light);
    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_RAW;
    item->name = sensor_list[arrayElement->total];
    item->value = light;
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_LONGLONG;
    item->name = TIMESTAMP;
    long long time = current_timestamp();
    item->value = (void *)&time;
    arrayElement->total++;
    
    tpSimpleTelemetry(arrayElement, 0);
    free(arrayElement->element);
    free(arrayElement);
    free(temp);
    free(humi);
    free(light);
}

static unsigned long getAvailableMemory() {
    unsigned long ps = sysconf(_SC_PAGESIZE);
    unsigned long pn = sysconf(_SC_AVPHYS_PAGES);
    unsigned long availMem = ps * pn;    
    return availMem;
}

static int getNetworkInfo(NetworkInfo* info, char* interface) {

    char* deviceIpAddress;
    char* gatewayIpAddress = NULL;
    char cmd [1000] = "";
    char line[256]= "";
    int sock;
    struct ifreq ifr;

    // device IP address
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return -1;
    }

    strcpy(ifr.ifr_name, interface);
    if (ioctl(sock, SIOCGIFADDR, &ifr)< 0)
    {
        close(sock);
        return -1;
    }
    deviceIpAddress = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    close(sock);
    memcpy(info->deviceIpAddress, deviceIpAddress, strlen(deviceIpAddress));
    
    // gateway IP address
    sprintf(cmd,"route -n | grep %s  | grep 'UG[ \t]' | awk '{print $2}'", interface);
    FILE* fp = popen(cmd, "r");
    if(fgets(line, sizeof(line), fp) != NULL) {
        gatewayIpAddress = &line[0];
    }
    pclose(fp);
    memcpy(info->gatewayIpAddress, gatewayIpAddress, strlen(gatewayIpAddress) - 1);

    return 0;
}

static void attribute() {

    ArrayElement* arrayElement = calloc(1, sizeof(ArrayElement));
    
    arrayElement->capacity = 15;
    arrayElement->element = calloc(1, sizeof(Element) * arrayElement->capacity);
    
    Element* item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_LONG;
    item->name = "sysAvailableMemory";
    unsigned long availableMemory = getAvailableMemory();
    item->value = &availableMemory;
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysFirmwareVersion";
    item->value = "2.0.0";
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysHardwareVersion";
    item->value = "1.0";
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysSerialNumber";
    item->value = "710DJC5I10000290";
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_LONG;
    item->name = "sysErrorCode";
    int errorCode = 0;
    item->value = &errorCode;
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysNetworkType";
    item->value = "ethernet";
    arrayElement->total++;

    NetworkInfo info;
    memset(&info, 0, sizeof(NetworkInfo));
    getNetworkInfo(&info, "eth0");
    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysDeviceIpAddress";
    item->value = info.deviceIpAddress;
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_STRING;
    item->name = "sysThingPlugIpAddress";
    item->value = MQTT_HOST;
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_RAW;
    item->name = "sysLocationLatitude";    
    item->value = "35.1689766";
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_RAW;
    item->name = "sysLocationLongitude";
    item->value = "129.1338524";
    arrayElement->total++;

    item = arrayElement->element + arrayElement->total;
    item->type = JSON_TYPE_LONG;
    item->name = "act7colorLed";
    int act7colorLed = 0;
    item->value = &act7colorLed;
    arrayElement->total++;

    tpSimpleAttribute(arrayElement);
    free(arrayElement->element);
    free(arrayElement);

    mStep = PROCESS_TELEMETRY;
}

/**
 * @brief get Device MAC Address without Colon.
 * @return mac address
 */
 char* GetMacAddressWithoutColon() {
    int i, sock;
    struct ifreq ifr;
    char mac_adr[18] = "";

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return NULL;
    }

    strcpy(ifr.ifr_name, "eth0");
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        close(sock);
        return NULL;
    }

    for (i = 0; i < 6; i++) {
        sprintf(&mac_adr[i*2],"%02X",((unsigned char*)ifr.ifr_hwaddr.sa_data)[i]);
    }
    close(sock);

    return strdup(mac_adr);
}


void start() {
    int rc;

    mConnectionStatus = CONNECTING;

    RGB_LEDControl(0);

    // set callbacks
    rc = tpMQTTSetCallbacks(MQTTConnected, MQTTSubscribed, MQTTDisconnected, MQTTConnectionLost, MQTTMessageDelivered, MQTTMessageArrived);
    SKTDebugPrint(LOG_LEVEL_INFO, "tpMQTTSetCallbacks result : %d", rc);
    // Simple SDK initialize
    rc = tpSimpleInitialize(SIMPLE_SERVICE_NAME, SIMPLE_DEVICE_NAME);
    SKTDebugPrint(LOG_LEVEL_INFO, "tpSimpleInitialize : %d", rc);
    // create clientID - MAC address
    char* macAddress = GetMacAddressWithoutColon();
    snprintf(mClientID, sizeof(mClientID), MQTT_CLIENT_ID, SIMPLE_DEVICE_NAME, macAddress);
    free(macAddress);
    SKTDebugPrint(LOG_LEVEL_INFO, "client id : %s", mClientID);
    // create Topics
    snprintf(mTopicControlDown, SIZE_TOPIC, MQTT_TOPIC_CONTROL_DOWN, SIMPLE_SERVICE_NAME, SIMPLE_DEVICE_NAME);

    char* subscribeTopics[] = { mTopicControlDown };

#if(MQTT_ENABLE_SERVER_CERT_AUTH)
	char host[] = MQTT_SECURE_HOST;
	int port = MQTT_SECURE_PORT;
#else
	char host[] = MQTT_HOST;
	int port = MQTT_PORT;
#endif
    rc = tpSDKCreate(host, port, MQTT_KEEP_ALIVE, LOGIN_NAME, NULL, 
        MQTT_ENABLE_SERVER_CERT_AUTH, subscribeTopics, TOPIC_SUBSCRIBE_SIZE, NULL, mClientID);
    SKTDebugPrint(LOG_LEVEL_INFO, "tpSDKCreate result : %d", rc);
}

int MARun() {
    SKTDebugInit(1, LOG_LEVEL_INFO, NULL);
	SKTDebugPrint(LOG_LEVEL_INFO, "ThingPlug_Simple_SDK");
    start();

    while (mStep < PROCESS_END) {
		if(tpMQTTIsConnected() && mStep == PROCESS_TELEMETRY) {
            telemetry();
        } 
        // reconnect when disconnected
        else if(mConnectionStatus == DISCONNECTED) {
            tpSDKDestroy();
            start();
        }

        #if defined(WIN32) || defined(WIN64)
            Sleep(100);
        #else
            usleep(10000000L);
        #endif
    }
    tpSDKDestroy();
    return 0;
}
