#ifndef _LIBRETRO_GUI_H_
#define _LIBRETRO_GUI_H_

#include <string>
#include <vector>
#include <map>
#include "../../Util/NewConfig.h"
#include "GameLoader.h"

class CLibretroInputSystem;

// Satisfies the call in LibretroWrapper::Emulate
std::vector<std::string> RunGUI(const std::string& configPath, Util::Config::Node& config);

// The actual frame-render call to be placed in your retro_run
void Libretro_UpdateGUI(Util::Config::Node& config, 
                        const std::map<std::string, Game>& games, 
                        bool& menuOpen, 
                        CLibretroInputSystem* inputSys);

void Libretro_ShutdownGUI();

#endif