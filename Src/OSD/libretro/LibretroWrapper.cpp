#include <iostream>
#include <new>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <memory>
#include <vector>
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

// --- External Audio Hooks ---
extern void PlayCallback(void *userdata, UINT8 *stream, int len);
extern UINT32 GetAvailableAudioLen();
extern int bytes_per_frame_host;

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

// Constants for synchronous audio
static const double MODEL3_FPS = 57.53;
static const int AUDIO_SAMPLE_RATE = 44100;
// Calculate bytes per frame once: (44100 / 57.53) * 4 bytes (stereo 16-bit)
static const int BYTES_PER_FRAME_SYNC = (int)((AUDIO_SAMPLE_RATE / MODEL3_FPS) * 4);

/*
 * Crosshair stuff
 */
static CCrosshair* s_crosshair = nullptr;

LibretroWrapper::LibretroWrapper() : 
    xRes(800), yRes(600), xOffset(0), yOffset(0), 
    totalXRes(800), totalYRes(600), aaValue(0)
{
      g_ctx = this;
}

LibretroWrapper::~LibretroWrapper() {}

void LibretroWrapper::InitializePaths(const std::string& baseConfigPath) 
{
    s_configFilePath   = baseConfigPath + "/Supermodel.ini";
    s_gameXMLFilePath  = baseConfigPath + "/Games.xml";
    s_musicXMLFilePath = baseConfigPath + "/Music.xml";
    s_logFilePath      = baseConfigPath + "/Supermodel.log";
    s_analysisPath     = baseConfigPath + "/Analysis/"; 

    std::cout << "[Supermodel] Paths remapped to: " << baseConfigPath << std::endl;
}

static void GLAPIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    printf("OGLDebug:: 0x%X: %s\n", id, message);
}

void LibretroWrapper::UpdateScreenSize(unsigned newWidth, unsigned newHeight)
{
    // If dimensions match and renderers are initialized, skip costly re-init
    if (newWidth == xRes && newHeight == yRes && superAA != nullptr)
        return;

    xRes = totalXRes = newWidth;
    yRes = totalYRes = newHeight;
    xOffset = 0;
    yOffset = 0;

    if (Model3)
        Model3->PauseThreads();

    InitRenderers();

    if (Model3)
        Model3->ResumeThreads();
}

void LibretroWrapper::SaveFrameBuffer(const std::string& file)
{
    std::shared_ptr<uint8_t> pixels(new uint8_t[totalXRes * totalYRes * 4], std::default_delete<uint8_t[]>());
    glReadPixels(0, 0, totalXRes, totalYRes, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
    Util::WriteSurfaceToBMP<Util::RGBA8>(file, pixels.get(), totalXRes, totalYRes, true);
}

void LibretroWrapper::Screenshot()
{
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
 Save States and NVRAM
******************************************************************************/

static const int STATE_FILE_VERSION = 5;  // save state file version
static const int NVRAM_FILE_VERSION = 0;  // NVRAM file version
static unsigned s_saveSlot = 0;           // save state slot #

static void SaveState(IEmulator *Model3)
{
  CBlockFile  SaveState;
  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;
  
  if (Result::OKAY != SaveState.Create(file_path, "Supermodel Save State", "Supermodel Version " SUPERMODEL_VERSION))
  {
    ErrorLog("Unable to save state to '%s'.", file_path.c_str());
    return;
  }

  int32_t fileVersion = STATE_FILE_VERSION;
  SaveState.Write(&fileVersion, sizeof(fileVersion));
  SaveState.Write(Model3->GetGame().name);

  Model3->SaveState(&SaveState);
  SaveState.Close();
  InfoLog("Saved state to '%s'.", file_path.c_str());
}

static void LoadState(IEmulator *Model3, std::string file_path = std::string())
{
  CBlockFile  SaveState;

  if (file_path.empty())
    file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Saves) << Model3->GetGame().name << ".st" << s_saveSlot;

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

  Model3->LoadState(&SaveState);
  SaveState.Close();
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

  int32_t fileVersion = NVRAM_FILE_VERSION;
  NVRAM.Write(&fileVersion, sizeof(fileVersion));
  NVRAM.Write(Model3->GetGame().name);

  Model3->SaveNVRAM(&NVRAM);
  NVRAM.Close();
}

static void LoadNVRAM(IEmulator *Model3)
{
  CBlockFile  NVRAM;
  std::string file_path = Util::Format() << FileSystemPath::GetPath(FileSystemPath::NVRAM) << Model3->GetGame().name << ".nv";

  if (Result::OKAY != NVRAM.Load(file_path)) return;

  if (Result::OKAY != NVRAM.FindBlock("Supermodel NVRAM State")) return;

  int32_t fileVersion;
  NVRAM.Read(&fileVersion, sizeof(fileVersion));
  if (fileVersion != NVRAM_FILE_VERSION) return;

  Model3->LoadNVRAM(&NVRAM);
  NVRAM.Close();
}

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
}

/******************************************************************************
 Frame Timing & Init
******************************************************************************/
bool LibretroWrapper::InitRenderers()
{
    delete Render2D; Render2D = nullptr;
    delete Render3D; Render3D = nullptr;
    delete superAA;  superAA  = nullptr;

    // Destroy previous libretro-managed FBO if any
    if (m_libretrFBO) {
        glDeleteFramebuffers(1,  &m_libretrFBO);   m_libretrFBO   = 0;
        glDeleteTextures(1,      &m_libretrTex);    m_libretrTex   = 0;
        glDeleteRenderbuffers(1, &m_libretrDepth);  m_libretrDepth = 0;
    }

    superAA = new SuperAA(aaValue, CRTcolors);
    superAA->Init(totalXRes, totalYRes);

    GLuint renderTarget = superAA->GetTargetID();

    // SuperAA skips FBO creation when aa==1 and no CRT filter.
    // In that case we must provide our own FBO; otherwise glBlitFramebuffer
    // reads from FBO-0 (the raw window backbuffer), which is invalid in libretro.
    if (renderTarget == 0)
    {
        glGenFramebuffers(1, &m_libretrFBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_libretrFBO);

        glGenTextures(1, &m_libretrTex);
        glBindTexture(GL_TEXTURE_2D, m_libretrTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     totalXRes, totalYRes,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_libretrTex, 0);

        glGenRenderbuffers(1, &m_libretrDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, m_libretrDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                              totalXRes, totalYRes);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, m_libretrDepth);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "[Supermodel] Libretro FBO incomplete: 0x%X\n", status);
        else
            fprintf(stderr, "[Supermodel] Libretro FBO created: %ux%u (id=%u)\n",
                    totalXRes, totalYRes, m_libretrFBO);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        renderTarget = m_libretrFBO;
    }

    Render2D = new CRender2D(s_runtime_config);
    Render3D =
        s_runtime_config["New3DEngine"].ValueAs<bool>()
        ? (IRender3D*)new New3D::CNew3D(s_runtime_config, Model3->GetGame().name)
        : (IRender3D*)new Legacy3D::CLegacy3D(s_runtime_config);

    if (Result::OKAY != Render2D->Init(
            xOffset*aaValue, yOffset*aaValue,
            xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            renderTarget, upscaleMode))   // ← use renderTarget, not superAA->GetTargetID()
        return false;

    if (Result::OKAY != Render3D->Init(
            xOffset*aaValue, yOffset*aaValue,
            xRes*aaValue, yRes*aaValue,
            totalXRes*aaValue, totalYRes*aaValue,
            renderTarget))               // ← same here
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

  MpegDec::LoadCustomTracks(s_musicXMLFilePath, game);

  totalXRes = xRes = s_runtime_config["XResolution"].ValueAs<unsigned>();
  totalYRes = yRes = s_runtime_config["YResolution"].ValueAs<unsigned>();
  snprintf(baseTitleStr, sizeof(baseTitleStr), "Supermodel - %s", game.title.c_str());

  SetAudioType(game.audio);
  if (Result::OKAY != OpenAudio(s_runtime_config))
    return 1;

  gameHasLightguns = !!(game.inputs & (Game::INPUT_GUN1|Game::INPUT_GUN2));
  gameHasLightguns |= game.name == "lostwsga";
  currentInputs = game.inputs;
  if (gameHasLightguns)
    videoInputs = Inputs;
  else
    videoInputs = nullptr;

  Model3->AttachInputs(Inputs);

  if (Outputs != nullptr)
    Model3->AttachOutputs(Outputs);

  Model3->Reset();

  if (!initialState.empty())
    LoadState(Model3, initialState);

  fpsFramesElapsed = 0;
  return 0;

QuitError:
  delete Render2D;
  delete Render3D;
  delete superAA;
  return 1;
}

int LibretroWrapper::Supermodel(const Game &game)
{
    if (paused)
    {
        Model3->RenderFrame();
    }
    else
    {
        Model3->RunFrame();

        // ULTRA-LIGHTWEIGHT: Calculate FPS only once every 60 frames
        static int frameCount = 0;
        static auto lastMeasureTime = std::chrono::steady_clock::now();
        static float cachedFPS = 57.53f;  // Start at target
        
        frameCount++;
        
        // Only recalculate FPS every 60 frames (basically once per second)
        if (frameCount >= 240)
        {
            auto currentTime = std::chrono::steady_clock::now();
            auto deltaTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                currentTime - lastMeasureTime
            ).count();
            
            if (deltaTime > 0) {
                // Average FPS over last 60 frames
                cachedFPS = (240.0f * 1000.0f) / (float)deltaTime;
            }
            
            lastMeasureTime = currentTime;
            frameCount = 0;
        }
        
        // Use the cached FPS value (only updated every 60 frames)
        float actualFPS = std::max(25.0f, std::min(60.0f, cachedFPS));
        int samplesThisFrame = (int)((44100.0f / actualFPS) + 0.5f);
        int bytesThisFrame = samplesThisFrame * 4;
        
        PlayCallback(NULL, NULL, bytesThisFrame);
    }

    if (Inputs->uiExit->Pressed())
    {
      quit = true;
    }
    else if (Inputs->uiReset->Pressed())
    {
      if (!paused) Model3->PauseThreads();
      Model3->Reset();
      if (!paused) Model3->ResumeThreads();
      puts("Model 3 reset.");
    }
    else if (Inputs->uiPause->Pressed())
    {
      paused = !paused;
      if (paused) Model3->PauseThreads();
      else        Model3->ResumeThreads();

      if (Outputs != NULL)
        Outputs->SetValue(OutputPause, paused);
    }
    else if (Inputs->uiSaveState->Pressed())
    {
      if (!paused) Model3->PauseThreads();
      SaveState(Model3);
      if (!paused) Model3->ResumeThreads();
    }
    else if (Inputs->uiChangeSlot->Pressed())
    {
      ++s_saveSlot;
      s_saveSlot %= 10;
      printf("Save slot: %d\n", s_saveSlot);
    }
    else if (Inputs->uiLoadState->Pressed())
    {
      if (!paused) Model3->PauseThreads();
      LoadState(Model3);
      if (!paused) Model3->ResumeThreads();
    }
    else if (Inputs->uiMusicVolUp->Pressed())
    {
      if (!Model3->GetGame().mpeg_board.empty()) {
        int vol = (std::min)(200, s_runtime_config["MusicVolume"].ValueAs<int>() + 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
      }
    }
    else if (Inputs->uiMusicVolDown->Pressed())
    {
      if (!Model3->GetGame().mpeg_board.empty()) {
        int vol = (std::max)(0, s_runtime_config["MusicVolume"].ValueAs<int>() - 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
      }
    }
    else if (Inputs->uiSoundVolUp->Pressed())
    {
      int vol = (std::min)(200, s_runtime_config["SoundVolume"].ValueAs<int>() + 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
    }
    else if (Inputs->uiSoundVolDown->Pressed())
    {
      int vol = (std::max)(0, s_runtime_config["SoundVolume"].ValueAs<int>() - 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
    }
#ifdef SUPERMODEL_DEBUGGER
    else if (Inputs->uiDumpInpState->Pressed())
    {
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
      Model3->ClearNVRAM();
      puts("NVRAM cleared.");
    }
    else if (Inputs->uiToggleFrLimit->Pressed())
    {
      s_runtime_config.Get("Throttle").SetValue(!s_runtime_config["Throttle"].ValueAs<bool>());
    }
    else if (Inputs->uiScreenshot->Pressed())
    {
      Screenshot();
    }

   if (s_runtime_config["ShowFrameRate"].ValueAs<bool>())
    {
      fpsFramesElapsed += 1;
    }

    if (dumpTimings && !paused)
    {
      CModel3 *M = dynamic_cast<CModel3 *>(Model3);
      if (M) M->DumpTimings();
    }

  return 0;
QuitError:
  return 1;
}

void LibretroWrapper::ShutDownSupermodel()
{
  Model3->PauseThreads();
  
  // NOTE: NVRAM is now saved by retro_unload_game() to the libretro buffer
  // Don't call SaveNVRAM() here - it would save to a file, which we don't want
  
  CloseAudio();

  delete Render2D;
  delete Render3D;
  delete superAA;
}

/******************************************************************************
 Entry Point and Command Line Processing
******************************************************************************/

static void WriteDefaultConfigurationFileIfNotPresent()
{
    FILE* fp = fopen(LibretroWrapper::s_configFilePath.c_str(), "r");
    if (fp) { fclose(fp); return; }

    fp = fopen(LibretroWrapper::s_configFilePath.c_str(), "w");
    if (!fp)
    {
        ErrorLog("Unable to write default configuration file to %s", LibretroWrapper::s_configFilePath.c_str());
        return;
    }
    fputs(s_defaultConfigFileContents, fp);
    fclose(fp);
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

  if (configure)
  {
    Util::Config::Node *fileConfigRoot = game.name.empty() ? fileConfig : fileConfig->TryGet(game.name);
    if (fileConfigRoot == nullptr)
      fileConfigRoot = &fileConfig->Add(game.name);

    if (Inputs->ConfigureInputs(game, xOffset, yOffset, xRes, yRes))
    {
      Inputs->StoreToConfig(fileConfigRoot);
      Util::Config::WriteINIFile(s_configFilePath, *fileConfig, configFileComment);
      Inputs->StoreToConfig(runtimeConfig);
    }
  }

  return Result::OKAY;
}

int LibretroWrapper::Emulate(const char* romPath) 
{
    WriteDefaultConfigurationFileIfNotPresent();
    SetLogger(std::make_shared<CConsoleErrorLogger>());

    char* argv[] = { (char*)"supermodel", (char*)romPath };
    int argc = 2;
    auto cmd_line = LibretroConfigProvider::ParseCommandLine(argc, argv);
    if (cmd_line.error) return 1;

    auto logger = CreateLogger(cmd_line.config);
    if (!logger) return 1;
    SetLogger(logger);
    InfoLog("Supermodel Version " SUPERMODEL_VERSION);
  
    bool rom_specified = !cmd_line.rom_files.empty();
    if (!rom_specified && !cmd_line.print_games && !cmd_line.config_inputs && !cmd_line.print_inputs)
    {
        ErrorLog("No ROM file specified.");
        return 0;
    }
    
    // Load and Merge Configuration
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
    Outputs = nullptr;

    aaValue = s_runtime_config["Supersampling"].ValueAs<int>();
    m_inputSystem = std::make_shared<CLibretroInputSystem>();
    InputSystem = m_inputSystem;

    Inputs = new CInputs(m_inputSystem);
    if (!Inputs->Initialize())
    {
        fprintf(stderr, "Failed to initialize Input System!\n");
        return 0; 
    }

    Model3 = new CModel3(s_runtime_config);

    if (ConfigureInputs(Inputs, &fileConfig, &s_runtime_config, game, cmd_line.config_inputs) != Result::OKAY)
    {
        exitCode = 1;
        goto Exit;
    }

    if (!rom_specified) goto Exit;

    // Fire up Supermodel
     this->rom_set = rom_set;
     this->Model3 = Model3;
     this->Inputs = Inputs;
     this->Outputs = Outputs;

Exit:
    return exitCode;
}

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

    // CRITICAL: Ensure internal textures (fonts, UI) are 1-byte aligned 
    // to match the fix in libretro.cpp. This prevents artifacts on legacy GL drivers.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    glDisable(GL_DITHER);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    InitRenderers();
}

GLuint LibretroWrapper::getSuperModelFBO() const 
{
    GLuint saaFBO = superAA ? superAA->GetTargetID() : 0;
    return (saaFBO != 0) ? saaFBO : m_libretrFBO;
}