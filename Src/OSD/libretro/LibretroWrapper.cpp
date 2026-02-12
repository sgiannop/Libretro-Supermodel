
#include <iostream>
#include <new>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <GL/glew.h>
#include <libretro.h>
#include "Inputs/Inputs.h"

#ifdef SUPERMODEL_WIN32
#include "DirectInputSystem.h"
#include "WinOutputs.h"
#endif

#include "Supermodel.h"
#include "Util/Format.h"
#include "Util/NewConfig.h"
#include "Util/ConfigBuilders.h"
#include "OSD/FileSystemPath.h"
#include "GameLoader.h"
#include "Debugger/SupermodelDebugger.h"
#include "Graphics/Legacy3D/Legacy3D.h"
#include "Graphics/New3D/New3D.h"
#include "Model3/IEmulator.h"
#include "Model3/Model3.h"
#include "OSD/Audio.h"
#include "Graphics/New3D/VBO.h"
#include "Graphics/SuperAA.h"
#include "Sound/MPEG/MpegAudio.h"
#include "Util/BMPFile.h"
#include "OSD/DefaultConfigFile.h"
#include "libretroCrosshair.h"
#include "LibretroWrapper.h"
#include "CLibretroInputSystem.h"
#include "libretroGui.h"
#include "LibretroConfigProvider.h"

/******************************************************************************
 Global Run-time Config
******************************************************************************/

std::string LibretroWrapper::s_analysisPath;
std::string LibretroWrapper::s_configFilePath;
std::string LibretroWrapper::s_gameXMLFilePath;
std::string LibretroWrapper::s_musicXMLFilePath;
std::string LibretroWrapper::s_logFilePath;

static Util::Config::Node s_runtime_config("Global");
static LibretroWrapper* g_ctx = nullptr;

/*
 * Crosshair stuff
 */
static CCrosshair* s_crosshair = nullptr;

void LibretroWrapper::InitializePaths(const std::string& baseConfigPath) 
{
    s_configFilePath  = baseConfigPath + "/Supermodel.ini";                                         // baseConfigPath is now something like "/home/user/retroarch/system/supermodel/Config"
    s_gameXMLFilePath = baseConfigPath + "/Games.xml";
    s_musicXMLFilePath = baseConfigPath + "/Music.xml";
    
    s_logFilePath     = baseConfigPath + "/Supermodel.log";                                         // For logs and analysis, we can stick them in a subfolder or the same place
    s_analysisPath    = baseConfigPath + "/Analysis/"; 

    std::cout << "[Supermodel] Paths remapped to: " << baseConfigPath << std::endl;
}

static void GLAPIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    printf("OGLDebug:: 0x%X: %s\n", id, message);
}

void LibretroWrapper::UpdateScreenSize(unsigned newWidth, unsigned newHeight)
{
    // If the size is the same, don't waste time re-initializing
    if (newWidth == xRes && newHeight == yRes && superAA != nullptr)
        return;

    xRes = totalXRes = newWidth;
    yRes = totalYRes = newHeight;

    // These calls tell Supermodel's Renderers to re-calculate their viewports
    if (superAA) superAA->Init(xRes, yRes);
    if (Render2D) Render2D->Init(0, 0, xRes*aaValue, yRes*aaValue, totalXRes*aaValue, totalYRes*aaValue, superAA->GetTargetID(), upscaleMode);
    if (Render3D) Render3D->Init(0, 0, xRes*aaValue, yRes*aaValue, totalXRes*aaValue, totalYRes*aaValue, superAA->GetTargetID());

    // CRITICAL: Force OpenGL to use the full area and stop clipping
    glViewport(0, 0, newWidth, newHeight);
    glScissor(0, 0, newWidth, newHeight);
    glDisable(GL_SCISSOR_TEST); 
}

void LibretroWrapper::SaveFrameBuffer(const std::string& file)
{
    std::shared_ptr<uint8_t> pixels(new uint8_t[totalXRes * totalYRes * 4], std::default_delete<uint8_t[]>());
    glReadPixels(0, 0, totalXRes, totalYRes, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
    Util::WriteSurfaceToBMP<Util::RGBA8>(file, pixels.get(), totalXRes, totalYRes, true);
}

void LibretroWrapper::Screenshot()
{
    // Make a screenshot
    time_t now = std::time(nullptr);
    tm* ltm = std::localtime(&now);
    std::string file = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Screenshots)
        << "Screenshot_" << std::setfill('0') << std::setw(4) << (1900 + ltm->tm_year)
        << '-' << std::setw(2) << (1 + ltm->tm_mon)
        << '-' << std::setw(2) << ltm->tm_mday
        << "_(" << std::setw(2) << ltm->tm_hour
        << '-' << std::setw(2) << ltm->tm_min
        << '-' << std::setw(2) << ltm->tm_sec
        << ").bmp";

    std::cout << "Screenshot created: " << file << std::endl;
    this->SaveFrameBuffer(file);
}

/******************************************************************************
 Render State Analysis
******************************************************************************/

/******************************************************************************
 Save States and NVRAM

 Save states and NVRAM use the same basic format. When anything changes that
 breaks compatibility with previous versions of Supermodel, the save state
 and NVRAM version numbers must be incremented as needed.

 Header block name: "Supermodel Save State" or "Supermodel NVRAM State"
 Data: Save state file version (4-byte integer), ROM set ID (up to 9 bytes,
 including terminating \0).

 Different subsystems output their own blocks.
******************************************************************************/

static const int STATE_FILE_VERSION = 5;  // save state file version
static const int NVRAM_FILE_VERSION = 0;  // NVRAM file version
static unsigned s_saveSlot = 0;           // save state slot #

LibretroWrapper::LibretroWrapper() : 
    xRes(800), yRes(600), xOffset(0), yOffset(0), 
    totalXRes(800), totalYRes(600), aaValue(0)
{
      g_ctx = this;
}

LibretroWrapper::~LibretroWrapper() {}

static void SaveState(IEmulator *Model3)
{
  CBlockFile  SaveState;

  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;
  if (Result::OKAY != SaveState.Create(file_path, "Supermodel Save State", "Supermodel Version " SUPERMODEL_VERSION))
  {
    ErrorLog("Unable to save state to '%s'.", file_path.c_str());
    return;
  }

  // Write file format version and ROM set ID to header block
  int32_t fileVersion = STATE_FILE_VERSION;
  SaveState.Write(&fileVersion, sizeof(fileVersion));
  SaveState.Write(Model3->GetGame().name);

  // Save state
  Model3->SaveState(&SaveState);
  SaveState.Close();
  printf("Saved state to '%s'.\n", file_path.c_str());
  InfoLog("Saved state to '%s'.", file_path.c_str());
}

static void LoadState(IEmulator *Model3, std::string file_path = std::string())
{
  CBlockFile  SaveState;

  // Generate file path
  if (file_path.empty())
    file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;

  // Open and check to make sure format is correct
  if (Result::OKAY != SaveState.Load(file_path))
  {
    ErrorLog("Unable to load state from '%s'.", file_path.c_str());
    return;
  }

  if (Result::OKAY != SaveState.FindBlock("Supermodel Save State"))
  {
    ErrorLog("'%s' does not appear to be a valid save state file.", file_path.c_str());
    return;
  }

  int32_t fileVersion;
  SaveState.Read(&fileVersion, sizeof(fileVersion));
  if (fileVersion != STATE_FILE_VERSION)
  {
    ErrorLog("'%s' is incompatible with this version of Supermodel.", file_path.c_str());
    return;
  }

  // Load
  Model3->LoadState(&SaveState);
  SaveState.Close();
  printf("Loaded state from '%s'.\n", file_path.c_str());
  InfoLog("Loaded state from '%s'.", file_path.c_str());
}

static void SaveNVRAM(IEmulator *Model3)
{
  CBlockFile  NVRAM;

  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::NVRAM) << Model3->GetGame().name << ".nv";
  if (Result::OKAY != NVRAM.Create(file_path, "Supermodel NVRAM State", "Supermodel Version " SUPERMODEL_VERSION))
  {
    ErrorLog("Unable to save NVRAM to '%s'. Make sure directory exists!", file_path.c_str());
    return;
  }

  // Write file format version and ROM set ID to header block
  int32_t fileVersion = NVRAM_FILE_VERSION;
  NVRAM.Write(&fileVersion, sizeof(fileVersion));
  NVRAM.Write(Model3->GetGame().name);

  // Save NVRAM
  Model3->SaveNVRAM(&NVRAM);
  NVRAM.Close();
  InfoLog("Saved NVRAM to '%s'.", file_path.c_str());
}

static void LoadNVRAM(IEmulator *Model3)
{
  CBlockFile  NVRAM;

  // Generate file path
  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::NVRAM) << Model3->GetGame().name << ".nv";

  // Open and check to make sure format is correct
  if (Result::OKAY != NVRAM.Load(file_path))
  {
    //ErrorLog("Unable to restore NVRAM from '%s'.", filePath);
    return;
  }

  if (Result::OKAY != NVRAM.FindBlock("Supermodel NVRAM State"))
  {
    ErrorLog("'%s' does not appear to be a valid NVRAM file.", file_path.c_str());
    return;
  }

  int32_t fileVersion;
  NVRAM.Read(&fileVersion, sizeof(fileVersion));
  if (fileVersion != NVRAM_FILE_VERSION)
  {
    ErrorLog("'%s' is incompatible with this version of Supermodel.", file_path.c_str());
    return;
  }

  // Load
  Model3->LoadNVRAM(&NVRAM);
  NVRAM.Close();
  InfoLog("Loaded NVRAM from '%s'.", file_path.c_str());
}

// void Supermodel_BindNvram(CModel3* model)
// {
//     // Backup RAM
//     model->backupRAM = g_saveRam + EEPROM_SIZE;

//     // EEPROM
//     model->EEPROM.SetRawBuffer(g_saveRam, EEPROM_SIZE);
// }

/******************************************************************************
 Video Callbacks
******************************************************************************/

static CInputs *videoInputs = NULL;
static uint32_t currentInputs = 0;

bool BeginFrameVideo()
{
  return true;
}

void EndFrameVideo()
{
  // Show crosshairs for light gun games
  if (videoInputs)
    s_crosshair->Update(currentInputs, videoInputs, g_ctx->getXOffset(), g_ctx->getYOffset(), g_ctx->getXRes(), g_ctx->getYRes());

  // Swap the buffers
  // SDL_GL_SwapWindow(g_ctx->getWindow());
}

/******************************************************************************
 Frame Timing
******************************************************************************/

static uint64_t s_perfCounterFrequency = 0;

static uint64_t GetDesiredRefreshRateMilliHz()
{
  // The refresh rate is expressed as mHz (millihertz -- Hz * 1000) in order to
  // be expressable as an integer. E.g.: 57.524 Hz -> 57524 mHz.
  float refreshRateHz = std::abs(s_runtime_config["RefreshRate"].ValueAs<float>());
  uint64_t refreshRateMilliHz = uint64_t(1000.0 * refreshRateHz);
  return refreshRateMilliHz;
}

bool LibretroWrapper::InitRenderers()
{
    // Delete old GL objects if they exist
    delete Render2D; Render2D = nullptr;
    delete Render3D; Render3D = nullptr;
    delete superAA;  superAA  = nullptr;

    superAA = new SuperAA(aaValue, CRTcolors);
    superAA->Init(totalXRes, totalYRes);

    Render2D = new CRender2D(s_runtime_config);
    Render3D =
        s_runtime_config["New3DEngine"].ValueAs<bool>()
        ? (IRender3D*)new New3D::CNew3D(s_runtime_config, Model3->GetGame().name)
        : (IRender3D*)new Legacy3D::CLegacy3D(s_runtime_config);

    if (Result::OKAY != Render2D->Init(
            xOffset*aaValue, yOffset*aaValue,
            xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            superAA->GetTargetID(), upscaleMode))
        return false;

    if (Result::OKAY != Render3D->Init(
            xOffset*aaValue, yOffset*aaValue,
            xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            superAA->GetTargetID()))
        return false;

    Model3->AttachRenderers(Render2D, Render3D, superAA);
    return true;
}

int LibretroWrapper::SuperModelInit(const Game &game) {

  initialState = s_runtime_config["InitStateFile"].ValueAs<std::string>();

  gameHasLightguns = false;
  quit = false;
  paused = false;
  dumpTimings = false;

  // Initialize and load ROMs
  if (Result::OKAY != Model3->Init())
    return 1;
  if (Model3->LoadGame(game, rom_set) != Result::OKAY)
    return 1;
  rom_set = ROMSet();  // free up this memory we won't need anymore

  // Customized music for games with MPEG boards
  MpegDec::LoadCustomTracks(s_musicXMLFilePath, game);

  // Load NVRAM
  //LoadNVRAM(Model3);

  // Set the video mode

  totalXRes = xRes = s_runtime_config["XResolution"].ValueAs<unsigned>();
  totalYRes = yRes = s_runtime_config["YResolution"].ValueAs<unsigned>();
  snprintf(baseTitleStr, sizeof(baseTitleStr), "Supermodel - %s", game.title.c_str());

  // int xpos = s_runtime_config["WindowXPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED);
  // int ypos = s_runtime_config["WindowYPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED);
  
  bool stretch = false;          
  bool fullscreen = false;
  SetAudioType(game.audio);
  if (Result::OKAY != OpenAudio(s_runtime_config))
    return 1;

  // Hide mouse if fullscreen, enable crosshairs for gun games
  //Inputs->GetInputSystem()->SetMouseVisibility(!s_runtime_config["FullScreen"].ValueAs<bool>());
  gameHasLightguns = !!(game.inputs & (Game::INPUT_GUN1|Game::INPUT_GUN2));
  gameHasLightguns |= game.name == "lostwsga";
  currentInputs = game.inputs;
  if (gameHasLightguns)
    videoInputs = Inputs;
  else
    videoInputs = nullptr;

  // Attach the inputs to the emulator
  Model3->AttachInputs(Inputs);

  // Attach the outputs to the emulator
  if (Outputs != nullptr)
    Model3->AttachOutputs(Outputs);

  // Reset emulator
  Model3->Reset();

  // Load initial save state if requested
  if (!initialState.empty())
    LoadState(Model3, initialState);

  fpsFramesElapsed = 0;
  quit = false;
  paused = false;
  dumpTimings = false;

  return 0;

QuitError:
  delete Render2D;
  delete Render3D;
  delete superAA;

  return 1;
}

int LibretroWrapper::Supermodel(const Game &game)
{
    extern void PlayCallback(void *userdata, UINT8 *stream, int len);
    extern UINT32 GetAvailableAudioLen(); // We'll create this helper
    extern int bytes_per_frame_host;
    if (paused)
        Model3->RenderFrame();
    else
    {
        Model3->RunFrame();

      // ALWAYS push exactly one frame. 
      // Because PlayCallback now pads with silence if the emulator is slow.
      int samples_to_push = 44100 / 57.53; // Approx 766 samples
      int bytes_to_push = samples_to_push * 4; // 2 channels * 16-bit
      PlayCallback(NULL, NULL, bytes_to_push);
    }

    if (Inputs->uiExit->Pressed())
    {
      quit = true;
    }
    else if (Inputs->uiReset->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
      }

      Model3->Reset();

      if (!paused)
      {
        Model3->ResumeThreads();
      }

      puts("Model 3 reset.");
    }
    else if (Inputs->uiPause->Pressed())
    {
      paused = !paused;                                 // Toggle emulator paused flag

      if (paused)
      {
        Model3->PauseThreads();
      }
      else
      {
        Model3->ResumeThreads();
      }

      if (Outputs != NULL)
        Outputs->SetValue(OutputPause, paused);
    }
    else if (Inputs->uiSaveState->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
      }

      
      SaveState(Model3);                                  // Save game state

      if (!paused)
      {
        Model3->ResumeThreads();
      }
    }
    else if (Inputs->uiChangeSlot->Pressed())
    {
      
      ++s_saveSlot;                                      // Change save slot
      s_saveSlot %= 10; // clamp to [0,9]
      printf("Save slot: %d\n", s_saveSlot);
    }
    else if (Inputs->uiLoadState->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
      }

      LoadState(Model3);                                // Load game state              

      if (!paused)
      {
        Model3->ResumeThreads();
      }
    }
    else if (Inputs->uiMusicVolUp->Pressed())
    {
      // Increase music volume by 10%
      if (!Model3->GetGame().mpeg_board.empty())
      {
        int vol = (std::min)(200, s_runtime_config["MusicVolume"].ValueAs<int>() + 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
        printf("Music volume: %d%%\n", vol);
      }
    }
    else if (Inputs->uiMusicVolDown->Pressed())
    {
      // Decrease music volume by 10%
      if (!Model3->GetGame().mpeg_board.empty())
      {
        int vol = (std::max)(0, s_runtime_config["MusicVolume"].ValueAs<int>() - 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
        printf("Music volume: %d%%\n", vol);
      }
    }
    else if (Inputs->uiSoundVolUp->Pressed())
    {
      // Increase sound volume by 10%
      int vol = (std::min)(200, s_runtime_config["SoundVolume"].ValueAs<int>() + 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
      printf("Sound volume: %d%%\n", vol);
    }
    else if (Inputs->uiSoundVolDown->Pressed())
    {
      // Decrease sound volume by 10%
      int vol = (std::max)(0, s_runtime_config["SoundVolume"].ValueAs<int>() - 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
      printf("Sound volume: %d%%\n", vol);
    }
#ifdef SUPERMODEL_DEBUGGER
    else if (Inputs->uiDumpInpState->Pressed())
    {
      // Dump input states
      Inputs->DumpState(&game);
    }
    else if (Inputs->uiDumpTimings->Pressed())
    {
      dumpTimings = !dumpTimings;
    }
#endif
    else if (Inputs->uiSelectCrosshairs->Pressed() && gameHasLightguns)
    {
      int crosshairs = (s_runtime_config["Crosshairs"].ValueAs<unsigned>() + 1) & 3;
      s_runtime_config.Get("Crosshairs").SetValue(crosshairs);
    }
    else if (Inputs->uiClearNVRAM->Pressed())
    {
      // Clear NVRAM
      Model3->ClearNVRAM();
      puts("NVRAM cleared.");
    }
    else if (Inputs->uiToggleFrLimit->Pressed())
    {
      // Toggle frame limiting
      s_runtime_config.Get("Throttle").SetValue(!s_runtime_config["Throttle"].ValueAs<bool>());
    }
    else if (Inputs->uiScreenshot->Pressed())
    {
      // Make a screenshot
      Screenshot();
    }

   if (s_runtime_config["ShowFrameRate"].ValueAs<bool>())
    {
      fpsFramesElapsed += 1;
    }

    if (dumpTimings && !paused)
    {
      CModel3 *M = dynamic_cast<CModel3 *>(Model3);
      if (M)
        M->DumpTimings();
    }

  return 0;

QuitError:
  return 1;
}

void LibretroWrapper::ShutDownSupermodel()
{
  // Make sure all threads are paused before shutting down
  Model3->PauseThreads();
  // Save NVRAM
  SaveNVRAM(Model3);

  // Close audio
  CloseAudio();

  // Shut down renderers
  delete Render2D;
  delete Render3D;
  delete superAA;
}

/******************************************************************************
 Entry Point and Command Line Processing
******************************************************************************/

// Configuration file is generated whenever it is not present. We no longer
// distribute it with Supermodel to make it easier to upgrade Supermodel from
// .zip archives without accidentally overwriting the configuration.
static void WriteDefaultConfigurationFileIfNotPresent()
{
    // Test whether file exists by opening it
    FILE* fp = fopen(LibretroWrapper::s_configFilePath.c_str(), "r");
    if (fp)
    {
        fclose(fp);
        return;
    }

    // Write config
    fp = fopen(LibretroWrapper::s_configFilePath.c_str(), "w");
    if (!fp)
    {
        ErrorLog("Unable to write default configuration file to %s", LibretroWrapper::s_configFilePath.c_str());
        return;
    }
    fputs(s_defaultConfigFileContents, fp);
    fclose(fp);
    InfoLog("Wrote default configuration file to %s", LibretroWrapper::s_configFilePath.c_str());
}

// Create and configure inputs
Result LibretroWrapper::ConfigureInputs(CInputs *Inputs, Util::Config::Node *fileConfig, Util::Config::Node *runtimeConfig, const Game &game, bool configure)
{
  static constexpr char configFileComment[] = {
    ";\n"
    "; Supermodel Configuration File\n"
    ";\n"
  };

  Inputs->LoadFromConfig(*runtimeConfig);

  // If the user wants to configure the inputs, do that now
  if (configure)
  {
    std::string title("Supermodel - ");
    if (game.name.empty())
      title.append("Configuring Default Inputs...");
    else
      title.append(Util::Format() << "Configuring Inputs for: " << game.title);
    // SDL_SetWindowTitle(s_window, title.c_str());

    // Extract the relevant INI section (which will be the global section if no
    // game was specified, otherwise the game's node) in the file config, which
    // will be written back to disk
    Util::Config::Node *fileConfigRoot = game.name.empty() ? fileConfig : fileConfig->TryGet(game.name);
    if (fileConfigRoot == nullptr)
    {
      fileConfigRoot = &fileConfig->Add(game.name);
    }

    // Configure the inputs
    if (Inputs->ConfigureInputs(game, xOffset, yOffset, xRes, yRes))
    {
      // Write input configuration and input system settings to config file
      Inputs->StoreToConfig(fileConfigRoot);
      Util::Config::WriteINIFile(s_configFilePath, *fileConfig, configFileComment);

      // Also save to runtime configuration in case we proceed and play
      Inputs->StoreToConfig(runtimeConfig);
    }
    else
      puts("Configuration aborted...");
    puts("");
  }

  return Result::OKAY;
}


static void LogConfig(const Util::Config::Node &config)
{
  InfoLog("Runtime configuration:");
  for (auto &child: config)
  {
    if (child.Empty())
      InfoLog("  %s=<empty>", child.Key().c_str());
    else
      InfoLog("  %s=%s", child.Key().c_str(), child.ValueAs<std::string>().c_str());
  }
  InfoLog("");
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

int LibretroWrapper::Emulate(const char* romPath) 
{
    WriteDefaultConfigurationFileIfNotPresent();

    bool loadGUI = false;

    // Before command line is parsed, console logging only
    SetLogger(std::make_shared<CConsoleErrorLogger>());

    // Load config and parse command line
    char* argv[] = { (char*)"supermodel", (char*)romPath };
    int argc = 2;
    auto cmd_line = ParseCommandLine(argc, argv);
    if (cmd_line.error)
    {
        return 1;
    }

    if (loadGUI) {
        Util::Config::Node fConfig1("Global");
        Util::Config::Node fConfig2("Global");

        Util::Config::FromINIFile(&fConfig1, s_configFilePath);
        Util::Config::MergeINISections(&fConfig2, LibretroConfigProvider::DefaultConfig(s_gameXMLFilePath), fConfig1); 

        cmd_line.rom_files = RunGUI(s_configFilePath, fConfig2);

        if (cmd_line.rom_files.empty()) {
            return 0;
        }
    }

    auto logger = CreateLogger(cmd_line.config);
    if (!logger)
    {
        ErrorLog("Unable to initialize logging system.");
        return 1;
    }
    SetLogger(logger);
    InfoLog("Supermodel Version " SUPERMODEL_VERSION);
  
   
    bool rom_specified = !cmd_line.rom_files.empty();
    if (!rom_specified && !cmd_line.print_games && !cmd_line.config_inputs && !cmd_line.print_inputs)
    {
        ErrorLog("No ROM file specified.");
        return 0;
    }
    
    Util::Config::Node fileConfig("Global");
    {
        Util::Config::Node fileConfigWithDefaults("Global");
        Util::Config::Node config3("Global");
        Util::Config::Node config4("Global");
        Util::Config::FromINIFile(&fileConfig, s_configFilePath);
        Util::Config::MergeINISections(&fileConfigWithDefaults, LibretroConfigProvider::DefaultConfig(s_gameXMLFilePath), fileConfig); 
        Util::Config::MergeINISections(&config3, fileConfigWithDefaults, cmd_line.config);    
        
        if (rom_specified || cmd_line.print_games)
        {
            std::string xml_file = config3["GameXMLFile"].ValueAs<std::string>();
            GameLoader loader(xml_file);
            if (loader.Load(&game, &rom_set, *cmd_line.rom_files.begin()))
                return 1;
            Util::Config::MergeINISections(&config4, config3, fileConfig[game.name]);   
        }
        else
            config4 = config3;
            
        Util::Config::MergeINISections(&s_runtime_config, config4, cmd_line.config);  
    }

    int exitCode = 0;
    IEmulator *Model3 = nullptr;
    std::shared_ptr<CInputSystem> InputSystem;
    // Inputs = nullptr;
    Outputs = nullptr;

    std::string selectedInputSystem = s_runtime_config["InputSystem"].ValueAs<std::string>();
    aaValue = s_runtime_config["Supersampling"].ValueAs<int>();
    m_inputSystem = std::make_shared<CLibretroInputSystem>();
    InputSystem = m_inputSystem;

    // 2. Create the CInputs Manager
    Inputs = new CInputs(m_inputSystem);                                                                      // We pass the hardware system to it.
    // 3. Initialize the Inputs
    if (!Inputs->Initialize())                                                                                // This sets up the default mappings and prepares the system.
    {
        // Log error if initialization fails
        fprintf(stderr, "Failed to initialize Input System!\n");
        return 0; 
    }

    Model3 = new CModel3(s_runtime_config);

    if (ConfigureInputs(Inputs, &fileConfig, &s_runtime_config, game, cmd_line.config_inputs) != Result::OKAY)
    {
        exitCode = 1;
        goto Exit;
    }

    if (!rom_specified)
        goto Exit;

    // Fire up Supermodel
     this->rom_set = rom_set;
     this->Model3 = Model3;
     this->Inputs = Inputs;
     this->Outputs = Outputs;
 
    //exitCode = Supermodel(game);
    //delete Model3;

Exit:
    // delete Inputs;
    // delete Outputs;
    // delete s_crosshair;

    return exitCode;
}
static bool gl_initialized = false;

void LibretroWrapper::InitGL()
{
    static bool glew_done = false;
    if (!glew_done)
    {
        GLenum err = glewInit();
        if (GLEW_OK != err)
        {
            ErrorLog("GLEW init failed: %s", glewGetErrorString(err));
            return;
        }
        glew_done = true;
    }

    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    // ONLY renderer recreation
    InitRenderers();   // delete + recreate Render2D/Render3D/superAA
}