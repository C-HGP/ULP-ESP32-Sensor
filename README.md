# ULP-ESP32-SENSOR

***What is ULP-ESP32-Sensor?***
This is a large project aimed at giving the HASS spectrum a relevant and smart temp-sensor.
It will give the user a easy UI to connect to the WiFi with HASS on, and help the user connect easily to HASS via MQTT.

***Stages***
The stages the unit will go into is:
* Boot and search for a known network, if not known, start a WiFi network.
* Let a user connect to the WiFi network.
* Receive credentials to the WiFi the device will use.
* Receive credentials to the MQTT broker that HASS is on.
* Receive how often the temp will be sent.

***To set up***
To set this up currently, you need knowledge in espress-idf, basic MQTT knowledge and HASS.
It will currently compile onto a ESP32 device with libraries noted in the folder, it does however have some bugs that
make the device unstable.

