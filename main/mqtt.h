#ifndef MQTT_H
#define MQTT_H

void mqtt_start();
void mqtt_publisher(const char * topic, const char * message);
void mqtt_publisher_qos(const char * topic, const char * message, const int qos);

#endif
