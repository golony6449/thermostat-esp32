// SCRUM-330: 본문은 src/firmware/thermostat_firmware.cpp에 있다.
// 스케치는 arduino-cli가 매 빌드마다 전처리·컴파일하므로 가볍게 둔다.
#include "thermostat_firmware.h"

void setup() { thermostat::firmware::setup(); }
void loop() { thermostat::firmware::loop(); }
