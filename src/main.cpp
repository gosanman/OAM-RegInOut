#include "Logic.h"
#include "GpioBinaryInputModule.h"
#include "SwitchActuatorModule.h"
#include "FileTransferModule.h"
#include "OpenKNX.h"
#include "hardware.h"

void setup()
{

    openknx.init();
    openknx.addModule(1, openknxLogic);
    openknx.addModule(2, openknxGpioBinaryInputModule);
    openknx.addModule(3, openknxSwitchActuatorModule);
    openknx.addModule(4, openknxFileTransferModule);
    openknx.setup();
}

void loop()
{
    openknx.loop();
}