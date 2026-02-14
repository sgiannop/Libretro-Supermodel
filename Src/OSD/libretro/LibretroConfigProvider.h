#pragma once
#include <Inputs/InputSystem.h>
#include "Util/NewConfig.h"

namespace LibretroConfigProvider {
     inline Util::Config::Node DefaultConfig(const std::string& gameXmlPath)
    {
        Util::Config::Node config("Global");
        
        config.Set("GameXMLFile", gameXmlPath);
        config.Set("InitStateFile", "");
        // CModel3
        config.Set("PowerPCFrequency", 0u, "Core", 0u, 200u);
        config.Set("MultiThreaded", true,"Core");
        config.Set("GPUMultiThreaded", true, "Core");
        // 2D and 3D graphics engines
        config.Set("MultiTexture", false, "Legacy3D");
        config.Set<std::string>("VertexShader", "", "Legacy3D", "", "");
        config.Set<std::string>("FragmentShader", "", "Legacy3D", "", "");
        
        // CSoundBoard
        config.Set("EmulateSound", true, "Sound");
        config.Set("Balance", 0.0f, "Sound", -100.f, 100.f);
        config.Set("BalanceLeftRight", 0.0f, "Sound", -100.f, 100.f);
        config.Set("BalanceFrontRear", 0.0f, "Sound", -100.f, 100.f);
        config.Set("NbSoundChannels", 4, "Sound", 0, 0, { 1,2,4 });
        config.Set("SoundFreq", 57.6f, "Sound", 0.0f, 0.0f, { 57.524160f, 60.f }); // 60.0f? 57.524160f?
        // CDSB
        
        config.Set("EmulateDSB", true, "Sound");
        config.Set("SoundVolume", 100, "Sound", 0, 200);
        config.Set("MusicVolume", 100, "Sound", 0, 200);
        // Other sound options
        config.Set("LegacySoundDSP", false, "Sound"); // New config option for games that do not play correctly with MAME's SCSP sound core.
        // CDriveBoard
        config.Set("ForceFeedback", false, "ForceFeedback");
        
        // Platform-specific/UI
        config.Set("New3DEngine", true, "Video");
        config.Set("QuadRendering", false, "Video");
        config.Set("XResolution", 496, "Video");
        config.Set("YResolution", 384, "Video");
        config.SetEmpty("WindowXPosition");
        config.SetEmpty("WindowYPosition");
        config.Set("FullScreen", false, "Video");
        config.Set("BorderlessWindow", false, "Video");
        config.Set("Supersampling", 1, "Video", 1, 8);
        config.Set("CRTcolors", int(0), "Video", 0, 0, { 0,1,2,3,4,5 });      // these might be more user friendly as strings
        config.Set("UpscaleMode", 2, "Video", 0, 0, { 0,1,2,3 });             // to do make strings
        config.Set("WideScreen", false, "Video");
        config.Set("Stretch", false, "Video");
        config.Set("WideBackground", false, "Video");
        config.Set("VSync", true, "Video");
        config.Set("Throttle", true, "Video");
        config.Set("RefreshRate", 60.0f, "Video", 0.0f, 0.0f, { 57.5f,60.f });
        config.Set("ShowFrameRate", false, "Video");
        config.Set("Crosshairs", int(0), "Video", 0, 0, { 0,1,2,3 });
        config.Set<std::string>("CrosshairStyle", "vector", "Video", "", "", { "bmp","vector" });
        config.Set("NoWhiteFlash", false, "Video");
        config.Set("FlipStereo", false, "Sound");
        #ifdef SUPERMODEL_WIN32
        config.Set<std::string>("InputSystem", "dinput", "Core", "", "", { "sdl","sdlgamepad","dinput","xinput","rawinput" });
        // DirectInput ForceFeedback
        config.Set("DirectInputConstForceLeftMax", 100, "ForceFeedback", 0, 100);
        config.Set("DirectInputConstForceRightMax", 100, "ForceFeedback", 0, 100);
        config.Set("DirectInputSelfCenterMax", 100, "ForceFeedback", 0, 100);
        config.Set("DirectInputFrictionMax", 100, "ForceFeedback", 0, 100);
        config.Set("DirectInputVibrateMax", 100, "ForceFeedback", 0, 100);
        // XInput ForceFeedback
        config.Set("XInputConstForceThreshold", 30, "ForceFeedback", 0, 100);
        config.Set("XInputConstForceMax", 100, "ForceFeedback", 0, 100);
        config.Set("XInputVibrateMax", 100, "ForceFeedback", 0, 100);
        config.Set("XInputStereoVibration", true, "ForceFeedback");
        // SDL ForceFeedback
        config.Set("SDLConstForceMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLSelfCenterMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLFrictionMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLVibrateMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLConstForceThreshold", 30, "ForceFeedback", 0, 100);
        #ifdef NET_BOARD

        // NetBoard
        config.Set("Network", false, "Network");
        config.Set("SimulateNet", true, "Network");
        config.Set("PortIn", unsigned(1970), "Network");
        config.Set("PortOut", unsigned(1971), "Network");
        config.Set<std::string>("AddressOut", "127.0.0.1", "Network", "", "");
        #endif
        #else
        config.Set<std::string>("InputSystem", "sdl", "Core", "", "", { "sdl","sdlgamepad" });
        // SDL ForceFeedback
        config.Set("SDLConstForceMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLSelfCenterMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLFrictionMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLVibrateMax", 100, "ForceFeedback", 0, 100);
        config.Set("SDLConstForceThreshold", 30, "ForceFeedback", 0, 100);
        #endif
        config.Set<std::string>("Outputs", "none", "Misc", "", "", { "none","win" });
        config.Set("DumpTextures", false, "Misc");

        //
        // Input sensitivity
        //
        config.Set<unsigned>("InputDigitalSensitivity", DEFAULT_DIGITAL_SENSITIVITY, "Sensitivity", 0, 100);
        config.Set<unsigned>("InputDigitalDecaySpeed", DEFAULT_DIGITAL_DECAYSPEED, "Sensitivity", 0, 100);
        config.Set<unsigned>("InputKeySensitivity", DEFAULT_DIGITAL_SENSITIVITY, "Sensitivity", 0, 100);
        config.Set<unsigned>("InputKeyDecaySpeed", DEFAULT_DIGITAL_DECAYSPEED, "Sensitivity", 0, 100);

        config.Set<unsigned>("InputMouseXDeadZone", DEFAULT_MSE_DEADZONE, "Sensitivity", 0, 100);
        config.Set<unsigned>("InputMouseYDeadZone", DEFAULT_MSE_DEADZONE, "Sensitivity", 0, 100);
        config.Set<unsigned>("InputMouseZDeadZone", DEFAULT_MSE_DEADZONE, "Sensitivity", 0, 100);

        //
        // controls
        //

        // Common
        config.Set<std::string>("InputStart1", "KEY_1,JOY1_BUTTON9", "Input", "", "");
        config.Set<std::string>("InputStart2", "KEY_2,JOY2_BUTTON9", "Input", "", "");
        config.Set<std::string>("InputCoin1", "KEY_3,JOY1_BUTTON10", "Input", "", "");
        config.Set<std::string>("InputCoin2", "KEY_4,JOY2_BUTTON10", "Input", "", "");
        config.Set<std::string>("InputServiceA", "KEY_5", "Input", "", "");
        config.Set<std::string>("InputServiceB", "KEY_7", "Input", "", "");
        config.Set<std::string>("InputTestA", "KEY_6", "Input", "", "");
        config.Set<std::string>("InputTestB", "KEY_8", "Input", "", "");

        // 4-way digital joysticks
        config.Set<std::string>("InputJoyUp", "KEY_UP,JOY1_UP", "Input", "", "");
        config.Set<std::string>("InputJoyDown", "KEY_DOWN,JOY1_DOWN", "Input", "", "");
        config.Set<std::string>("InputJoyLeft", "KEY_LEFT,JOY1_LEFT", "Input", "", "");
        config.Set<std::string>("InputJoyRight", "KEY_RIGHT,JOY1_RIGHT", "Input", "", "");
        config.Set<std::string>("InputJoyUp2", "JOY2_UP", "Input", "", "");
        config.Set<std::string>("InputJoyDown2", "JOY2_DOWN", "Input", "", "");
        config.Set<std::string>("InputJoyLeft2", "JOY2_LEFT", "Input", "", "");
        config.Set<std::string>("InputJoyRight2", "JOY2_RIGHT", "Input", "", "");

        // Fighting game buttons
        config.Set<std::string>("InputPunch", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputKick", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputGuard", "KEY_D,JOY1_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputEscape", "KEY_F,JOY1_BUTTON4", "Input", "", "");
        config.Set<std::string>("InputPunch2", "JOY2_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputKick2", "JOY2_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputGuard2", "JOY2_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputEscape2", "JOY2_BUTTON4", "Input", "", "");

        // Spikeout buttons
        config.Set<std::string>("InputShift", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputBeat", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputCharge", "KEY_D,JOY1_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputJump", "KEY_F,JOY1_BUTTON4", "Input", "", "");

        // Virtua Striker buttons
        config.Set<std::string>("InputShortPass", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputLongPass", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputShoot", "KEY_D,JOY1_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputShortPass2", "JOY2_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputLongPass2", "JOY2_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputShoot2", "JOY2_BUTTON3", "Input", "", "");

        // Steering wheel
        config.Set<std::string>("InputSteeringLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputSteeringRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputSteering", "JOY1_XAXIS", "Input", "", "");

        // Pedals
        config.Set<std::string>("InputAccelerator", "KEY_UP,JOY1_UP", "Input", "", "");
        config.Set<std::string>("InputBrake", "KEY_DOWN,JOY1_DOWN", "Input", "", "");

        // Up/down shifter manual transmission (all racers)
        config.Set<std::string>("InputGearShiftUp", "KEY_Y", "Input", "", "");
        config.Set<std::string>("InputGearShiftDown", "KEY_H", "Input", "", "");

        // 4-Speed manual transmission (Daytona 2, Sega Rally 2, Scud Race)
        config.Set<std::string>("InputGearShift1", "KEY_Q,JOY1_BUTTON5", "Input", "", "");
        config.Set<std::string>("InputGearShift2", "KEY_W,JOY1_BUTTON6", "Input", "", "");
        config.Set<std::string>("InputGearShift3", "KEY_E,JOY1_BUTTON7", "Input", "", "");
        config.Set<std::string>("InputGearShift4", "KEY_R,JOY1_BUTTON8", "Input", "", "");
        config.Set<std::string>("InputGearShiftN", "KEY_T", "Input", "", "");

        // VR4 view change buttons (Daytona 2, Le Mans 24, Scud Race)
        config.Set<std::string>("InputVR1", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputVR2", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputVR3", "KEY_D,JOY1_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputVR4", "KEY_F,JOY1_BUTTON4", "Input", "", "");

        // Single view change button (Dirt Devils, ECA, Harley-Davidson, Sega Rally 2)
        config.Set<std::string>("InputViewChange", "KEY_A,JOY1_BUTTON1", "Input", "", "");

        // Handbrake (Sega Rally 2)
        config.Set<std::string>("InputHandBrake", "KEY_S,JOY1_BUTTON2", "Input", "", "");

        // Harley-Davidson controls
        config.Set<std::string>("InputRearBrake", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputMusicSelect", "KEY_D,JOY1_BUTTON3", "Input", "", "");

        // Virtual On macros
        config.Set<std::string>("InputTwinJoyTurnLeft", "KEY_Q,JOY1_RXAXIS_NEG", "Input", "", "");
        config.Set<std::string>("InputTwinJoyTurnRight", "KEY_W,JOY1_RXAXIS_POS", "Input", "", "");
        config.Set<std::string>("InputTwinJoyForward", "KEY_UP,JOY1_YAXIS_NEG", "Input", "", "");
        config.Set<std::string>("InputTwinJoyReverse", "KEY_DOWN,JOY1_YAXIS_POS", "Input", "", "");
        config.Set<std::string>("InputTwinJoyStrafeLeft", "KEY_LEFT,JOY1_XAXIS_NEG", "Input", "", "");
        config.Set<std::string>("InputTwinJoyStrafeRight", "KEY_RIGHT,JOY1_XAXIS_POS", "Input", "", "");
        config.Set<std::string>("InputTwinJoyJump", "KEY_E,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputTwinJoyCrouch", "KEY_R,JOY1_BUTTON2", "Input", "", "");

        // Virtual On individual joystick mapping
        config.Set<std::string>("InputTwinJoyLeft1", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyLeft2", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyRight1", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyRight2", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyUp1", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyUp2", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyDown1", "NONE", "Input", "", "");
        config.Set<std::string>("InputTwinJoyDown2", "NONE", "Input", "", "");

        // Virtual On buttons
        config.Set<std::string>("InputTwinJoyShot1", "KEY_A,JOY1_BUTTON5", "Input", "", "");
        config.Set<std::string>("InputTwinJoyShot2", "KEY_S,JOY1_BUTTON6", "Input", "", "");
        config.Set<std::string>("InputTwinJoyTurbo1", "KEY_Z,JOY1_BUTTON7", "Input", "", "");
        config.Set<std::string>("InputTwinJoyTurbo2", "KEY_X,JOY1_BUTTON8", "Input", "", "");

        // Analog joystick (Star Wars Trilogy)
        config.Set<std::string>("InputAnalogJoyLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyUp", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyDown", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyX", "JOY_XAXIS,MOUSE_XAXIS", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyY", "JOY_YAXIS,MOUSE_YAXIS", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyTrigger", "KEY_A,JOY_BUTTON1,MOUSE_LEFT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyEvent", "KEY_S,JOY_BUTTON2,MOUSE_RIGHT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyTrigger2", "KEY_D,JOY_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputAnalogJoyEvent2", "NONE", "Input", "", "");

        // Light guns (Lost World)
        config.Set<std::string>("InputGunLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputGunRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputGunUp", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputGunDown", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputGunX", "MOUSE_XAXIS,JOY1_XAXIS", "Input", "", "");
        config.Set<std::string>("InputGunY", "MOUSE_YAXIS,JOY1_YAXIS", "Input", "", "");
        config.Set<std::string>("InputTrigger", "KEY_A,JOY1_BUTTON1,MOUSE_LEFT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputOffscreen", "KEY_S,JOY1_BUTTON2,MOUSE_RIGHT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputAutoTrigger", "0", "Input", "", "");
        config.Set<std::string>("InputGunLeft2", "NONE", "Input", "", "");
        config.Set<std::string>("InputGunRight2", "NONE", "Input", "", "");
        config.Set<std::string>("InputGunUp2", "NONE", "Input", "", "");
        config.Set<std::string>("InputGunDown2", "NONE", "Input", "", "");
        config.Set<std::string>("InputGunX2", "JOY2_XAXIS", "Input", "", "");
        config.Set<std::string>("InputGunY2", "JOY2_YAXIS", "Input", "", "");
        config.Set<std::string>("InputTrigger2", "JOY2_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputOffscreen2", "JOY2_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputAutoTrigger2", "0", "Input", "", "");

        // Analog guns (Ocean Hunter, LA Machineguns)
        config.Set<std::string>("InputAnalogGunLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputAnalogGunRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputAnalogGunUp", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputAnalogGunDown", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputAnalogGunX", "MOUSE_XAXIS,JOY1_XAXIS", "Input", "", "");
        config.Set<std::string>("InputAnalogGunY", "MOUSE_YAXIS,JOY1_YAXIS", "Input", "", "");
        config.Set<std::string>("InputAnalogTriggerLeft", "KEY_A,JOY1_BUTTON1,MOUSE_LEFT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputAnalogTriggerRight", "KEY_S,JOY1_BUTTON2,MOUSE_RIGHT_BUTTON", "Input", "", "");
        config.Set<std::string>("InputAnalogGunLeft2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogGunRight2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogGunUp2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogGunDown2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogGunX2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogGunY2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogTriggerLeft2", "NONE", "Input", "", "");
        config.Set<std::string>("InputAnalogTriggerRight2", "NONE", "Input", "", "");

        // Ski Champ controls
        config.Set<std::string>("InputSkiLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputSkiRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputSkiUp", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputSkiDown", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputSkiX", "JOY1_XAXIS", "Input", "", "");
        config.Set<std::string>("InputSkiY", "JOY1_YAXIS", "Input", "", "");
        config.Set<std::string>("InputSkiPollLeft", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputSkiPollRight", "KEY_S,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputSkiSelect1", "KEY_Q,JOY1_BUTTON3", "Input", "", "");
        config.Set<std::string>("InputSkiSelect2", "KEY_W,JOY1_BUTTON4", "Input", "", "");
        config.Set<std::string>("InputSkiSelect3", "KEY_E,JOY1_BUTTON5", "Input", "", "");

        // Magical Truck Adventure controls
        config.Set<std::string>("InputMagicalLeverUp1", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputMagicalLeverDown1", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputMagicalLeverUp2", "NONE", "Input", "", "");
        config.Set<std::string>("InputMagicalLeverDown2", "NONE", "Input", "", "");
        config.Set<std::string>("InputMagicalLever1", "JOY1_YAXIS", "Input", "", "");
        config.Set<std::string>("InputMagicalLever2", "JOY2_YAXIS", "Input", "", "");
        config.Set<std::string>("InputMagicalPedal1", "KEY_A,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputMagicalPedal2", "KEY_S,JOY2_BUTTON1", "Input", "", "");

        // Sega Bass Fishing / Get Bass controls
        config.Set<std::string>("InputFishingRodLeft", "KEY_LEFT", "Input", "", "");
        config.Set<std::string>("InputFishingRodRight", "KEY_RIGHT", "Input", "", "");
        config.Set<std::string>("InputFishingRodUp", "KEY_UP", "Input", "", "");
        config.Set<std::string>("InputFishingRodDown", "KEY_DOWN", "Input", "", "");
        config.Set<std::string>("InputFishingStickLeft", "KEY_A", "Input", "", "");
        config.Set<std::string>("InputFishingStickRight", "KEY_D", "Input", "", "");
        config.Set<std::string>("InputFishingStickUp", "KEY_W", "Input", "", "");
        config.Set<std::string>("InputFishingStickDown", "KEY_S", "Input", "", "");
        config.Set<std::string>("InputFishingRodX", "JOY1_XAXIS", "Input", "", "");
        config.Set<std::string>("InputFishingRodY", "JOY1_YAXIS", "Input", "", "");
        config.Set<std::string>("InputFishingStickX", "JOY1_RXAXIS", "Input", "", "");
        config.Set<std::string>("InputFishingStickY", "JOY1_RYAXIS", "Input", "", "");
        config.Set<std::string>("InputFishingReel", "KEY_SPACE,JOY1_ZAXIS_POS", "Input", "", "");
        config.Set<std::string>("InputFishingCast", "KEY_Z,JOY1_BUTTON1", "Input", "", "");
        config.Set<std::string>("InputFishingSelect", "KEY_X,JOY1_BUTTON2", "Input", "", "");
        config.Set<std::string>("InputFishingTension", "KEY_T,JOY1_ZAXIS_NEG", "Input", "", "");

        return config;
    }

    struct ParsedCommandLine
    {
        Util::Config::Node config = Util::Config::Node("CommandLine");
        std::vector<std::string> rom_files;
        bool error = false;
        bool print_help = false;
        bool print_games = false;
        bool print_gl_info = false;
        bool config_inputs = false;
        bool print_inputs = false;
        bool disable_debugger = false;
        bool enter_debugger = false;
        #ifdef DEBUG
        std::string gfx_state;
        #endif

        ParsedCommandLine()
        {
            // Logging is special: it is only parsed from the command line and
            // therefore, defaults are needed early
            config.Set("LogOutput", LibretroWrapper::s_logFilePath.c_str());
            config.Set("LogLevel", "info");
        }
            
    };

    static ParsedCommandLine ParseCommandLine(int argc, char **argv)
    {
    ParsedCommandLine cmd_line;
    static const std::map<std::string, std::string> valued_options
    { // -option=value
        { "-game-xml-file",         "GameXMLFile"             },
        { "-load-state",            "InitStateFile"           },
        { "-ppc-frequency",         "PowerPCFrequency"        },
        { "-crosshairs",            "Crosshairs"              },
        { "-crosshair-style",       "CrosshairStyle"          },
        { "-vert-shader",           "VertexShader"            },
        { "-frag-shader",           "FragmentShader"          },
        { "-sound-volume",          "SoundVolume"             },
        { "-music-volume",          "MusicVolume"             },
        { "-balance",               "Balance"                 },
        { "-channels", 	            "NbSoundChannels"         },
        { "-soundfreq",             "SoundFreq"               },
        { "-input-system",          "InputSystem"             },
        { "-outputs",               "Outputs"                 },
        { "-log-output",            "LogOutput"               },
        { "-log-level",             "LogLevel"                }
    };

    static const std::map<std::string, std::pair<std::string, bool>> bool_options
    { // -option
        { "-threads",             { "MultiThreaded",    true } },
        { "-no-threads",          { "MultiThreaded",    false } },
        { "-gpu-multi-threaded",  { "GPUMultiThreaded", true } },
        { "-no-gpu-thread",       { "GPUMultiThreaded", false } },
        { "-window",              { "FullScreen",       false } },
        { "-fullscreen",          { "FullScreen",       true } },
        { "-borderless",          { "BorderlessWindow", true } },
        { "-no-wide-screen",      { "WideScreen",       false } },
        { "-wide-screen",         { "WideScreen",       true } },
        { "-stretch",             { "Stretch",          true } },
        { "-no-stretch",          { "Stretch",          false } },
        { "-wide-bg",             { "WideBackground",   true } },
        { "-no-wide-bg",          { "WideBackground",   false } },
        { "-no-multi-texture",    { "MultiTexture",     false } },
        { "-multi-texture",       { "MultiTexture",     true } },
        { "-throttle",            { "Throttle",         true } },
        { "-no-throttle",         { "Throttle",         false } },
        { "-vsync",               { "VSync",            true } },
        { "-no-vsync",            { "VSync",            false } },
        { "-show-fps",            { "ShowFrameRate",    true } },
        { "-no-fps",              { "ShowFrameRate",    false } },
        { "-new3d",               { "New3DEngine",      true } },
        { "-quad-rendering",      { "QuadRendering",    true } },
        { "-legacy3d",            { "New3DEngine",      false } },
        { "-no-flip-stereo",      { "FlipStereo",       false } },
        { "-flip-stereo",         { "FlipStereo",       true } },
        { "-sound",               { "EmulateSound",     true } },
        { "-no-sound",            { "EmulateSound",     false } },
        { "-dsb",                 { "EmulateDSB",       true } },
        { "-no-dsb",              { "EmulateDSB",       false } },
        { "-legacy-scsp",         { "LegacySoundDSP",   true } },
        { "-new-scsp",            { "LegacySoundDSP",   false } },
        { "-no-white-flash",      { "NoWhiteFlash",     true } },
        { "-white-flash",         { "NoWhiteFlash",     false } },
    #ifdef NET_BOARD
        { "-net",                 { "Network",       true } },
        { "-no-net",              { "Network",       false } },
        { "-simulate-netboard",   { "SimulateNet",   true } },
        { "-emulate-netboard",    { "SimulateNet",   false } },
    #endif
        { "-no-force-feedback",   { "ForceFeedback",    false } },
        { "-force-feedback",      { "ForceFeedback",    true } },
        { "-dump-textures",       { "DumpTextures",     true } },
    };
    for (int i = 1; i < argc; i++)
    {
        std::string arg(argv[i]);
        if (arg[0] == '-')
        {
        // First, check maps
        size_t idx_equals = arg.find_first_of('=');
        if (idx_equals != std::string::npos)
        {
            std::string option(arg.begin(), arg.begin() + idx_equals);
            std::string value(arg.begin() + idx_equals + 1, arg.end());
            if (value.empty())
            {
            ErrorLog("Argument to '%s' cannot be blank.", option.c_str());
            cmd_line.error = true;
            continue;
            }
            auto it = valued_options.find(option);
            if (it != valued_options.end())
            {
            const std::string &config_key = it->second;
            cmd_line.config.Set(config_key, value);
            continue;
            }
        }
        else
        {
            auto it = bool_options.find(arg);
            if (it != bool_options.end())
            {
            const std::string &config_key = it->second.first;
            bool value = it->second.second;
            cmd_line.config.Set(config_key, value);
            continue;
            }
            else if (valued_options.find(arg) != valued_options.end())
            {
            ErrorLog("'%s' requires an argument.", argv[i]);
            cmd_line.error = true;
            continue;
            }
        }
        // Fell through -- handle special cases
        if (arg == "-?" || arg == "-h" || arg == "-help" || arg == "--help")
            cmd_line.print_help = true;
        else if (arg == "-print-games")
            cmd_line.print_games = true;
        else if (arg == "-res" || arg.find("-res=") == 0)
        {
            std::vector<std::string> parts = Util::Format(arg).Split('=');
            if (parts.size() != 2)
            {ErrorLog("'-res' requires both a width and height (e.g., '-res=496,384').");
            cmd_line.error = true;
            }
            else
            {
            unsigned  x, y;
            if (2 == sscanf(&argv[i][4],"=%u,%u", &x, &y))
            {
                std::string xres = Util::Format() << x;
                std::string yres = Util::Format() << y;
                cmd_line.config.Set("XResolution", xres);
                cmd_line.config.Set("YResolution", yres);
            }
            else
            {
                ErrorLog("'-res' requires both a width and height (e.g., '-res=496,384').");
                cmd_line.error = true;
            }
            }
        }
        else if (arg == "-window-pos" || arg.find("-window-pos=") == 0)
        {
            std::vector<std::string> parts = Util::Format(arg).Split('=');
            if (parts.size() != 2)
            {
                ErrorLog("'-window-pos' requires both an X and Y position (e.g., '-window-pos=10,0').");
                cmd_line.error = true;
            }
            else
            {
                int xpos, ypos;
                if (2 == sscanf(&argv[i][11], "=%d,%d", &xpos, &ypos))
                {
                    cmd_line.config.Set("WindowXPosition", xpos);
                    cmd_line.config.Set("WindowYPosition", ypos);
                }
                else
                {
                    ErrorLog("'-window-pos' requires both an X and Y position (e.g., '-window-pos=10,0').");
                    cmd_line.error = true;
                }
            }
        }
        else if (arg == "-ss" || arg.find("-ss=") == 0) {

            std::vector<std::string> parts = Util::Format(arg).Split('=');

            if (parts.size() != 2)
            {
                ErrorLog("'-ss' requires an integer argument (e.g., '-ss=2').");
                cmd_line.error = true;
            }
            else {

                try {
                    int val = std::stoi(parts[1]);
                    val = std::clamp(val, 1, 8);

                    cmd_line.config.Set("Supersampling", val);
                }
                catch (...) {
                    ErrorLog("'-ss' requires an integer argument (e.g., '-ss=2').");
                    cmd_line.error = true;
                }
            }
        }
        else if (arg == "-crtcolors" || arg.find("-crtcolors=") == 0) {

            std::vector<std::string> parts = Util::Format(arg).Split('=');

            if (parts.size() != 2)
            {
                ErrorLog("'-crtcolors' requires an integer argument (e.g., '-crtcolors=1').");
                cmd_line.error = true;
            }
            else {

                try {
                    int val = std::stoi(parts[1]);
                    val = std::clamp(val, 0, 5);

                    cmd_line.config.Set("CRTcolors", val);
                }
                catch (...) {
                    ErrorLog("'-crtcolors' requires an integer argument (e.g., '-crtcolors=1').");
                    cmd_line.error = true;
                }
            }
        }
        else if (arg == "-upscalemode" || arg.find("-upscalemode=") == 0) {

            std::vector<std::string> parts = Util::Format(arg).Split('=');

            if (parts.size() != 2)
            {
                ErrorLog("'-upscalemode' requires an integer argument (e.g., '-upscalemode=1').");
                cmd_line.error = true;
            }
            else {

                try {
                    int val = std::stoi(parts[1]);
                    val = std::clamp(val, 0, 3);

                    cmd_line.config.Set("UpscaleMode", val);
                }
                catch (...) {
                    ErrorLog("'-upscalemode' requires an integer argument (e.g., '-upscalemode=1').");
                    cmd_line.error = true;
                }
            }
        }
        else if (arg == "-true-hz")
            cmd_line.config.Set("RefreshRate", 57.524f);
        else if (arg == "-print-gl-info")
            cmd_line.print_gl_info = true;
        else if (arg == "-config-inputs")
            cmd_line.config_inputs = true;
        else if (arg == "-print-inputs")
            cmd_line.print_inputs = true;
        else
        {
            ErrorLog("Ignoring unrecognized option: %s", argv[i]);
            cmd_line.error = true;
        }
        }
        else
        cmd_line.rom_files.emplace_back(arg);
    }
    return cmd_line;
    }
}