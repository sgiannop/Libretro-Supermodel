
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
#include <SDL2/SDL_video.h>
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

/******************************************************************************
 Global Run-time Config
******************************************************************************/

static const std::string s_analysisPath = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Analysis);
static const std::string s_configFilePath = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Config) << "Supermodel.ini";
static const std::string s_gameXMLFilePath = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Config) << "Games.xml";
static const std::string s_musicXMLFilePath = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Config) << "Music.xml";
static const std::string s_logFilePath = Util::Format() << FileSystemPath::GetPath(FileSystemPath::Log) << "Supermodel.log";

static Util::Config::Node s_runtime_config("Global");
static LibretroWrapper* g_ctx = nullptr;

/*
 * Crosshair stuff
 */
static CCrosshair* s_crosshair = nullptr;

Result LibretroWrapper::SetGLGeometry(unsigned *xOffsetPtr, unsigned *yOffsetPtr, unsigned *xResPtr, unsigned *yResPtr, unsigned *totalXResPtr, unsigned *totalYResPtr, bool keepAspectRatio)
{
  // What resolution did we actually get?
  int actualWidth;
  int actualHeight;
  SDL_GetWindowSize(s_window, &actualWidth, &actualHeight);
  *totalXResPtr = actualWidth;
  *totalYResPtr = actualHeight;

  // If required, fix the aspect ratio of the resolution that the user passed to match Model 3 ratio
  float xResF = float(*xResPtr);
  float yResF = float(*yResPtr);
  if (keepAspectRatio)
  {
    float model3Ratio = float(496.0/384.0);
    if (yResF < (xResF/model3Ratio))
      xResF = yResF*model3Ratio;
    if (xResF < (yResF*model3Ratio))
      yResF = xResF/model3Ratio;
  }

  // Center the visible area
  *xOffsetPtr = (*xResPtr - (unsigned) xResF)/2;
  *yOffsetPtr = (*yResPtr - (unsigned) yResF)/2;

  // If the desired resolution is smaller than what we got, re-center again
  if (int(*xResPtr) < actualWidth)
    *xOffsetPtr += (actualWidth - *xResPtr)/2;
  if (int(*yResPtr) < actualHeight)
    *yOffsetPtr += (actualHeight - *yResPtr)/2;

  // OpenGL initialization
  glViewport(0,0,*xResPtr,*yResPtr);
  glClearColor(0.0,0.0,0.0,0.0);
  glClearDepth(1.0);
  glDepthFunc(GL_LESS);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  // Clear both buffers to ensure a black border
  for (int i = 0; i < 3; i++)
  {
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    SDL_GL_SwapWindow(s_window);
  }

  // Write back resolution parameters
  *xResPtr = (unsigned) xResF;
  *yResPtr = (unsigned) yResF;

  UINT32 correction = (UINT32)(((*yResPtr / 384.) * 2.) + 0.5); // due to the 2D layer compensation (2 pixels off)

  glEnable(GL_SCISSOR_TEST);

  // Scissor box (to clip visible area)
  if (s_runtime_config["WideScreen"].ValueAsDefault<bool>(false))
  {
    glScissor(0* aaValue, correction* aaValue, *totalXResPtr * aaValue, (*totalYResPtr - (correction * 2)) * aaValue);
  }
  else
  {
    glScissor((*xOffsetPtr + correction) * aaValue, (*yOffsetPtr + correction) * aaValue, (*xResPtr - (correction * 2)) * aaValue, (*yResPtr - (correction * 2)) * aaValue);
  }
  return Result::OKAY;
}

static void GLAPIENTRY DebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    printf("OGLDebug:: 0x%X: %s\n", id, message);
}

// In windows with an nvidia card (sorry not tested anything else) you can customise the resolution.
// This also allows you to set a totally custom refresh rate. Apparently you can drive most monitors at
// 57.5fps with no issues. Anyway this code will automatically pick up your custom refresh rate, and set it if it exists.
// If it doesn't exist, then it'll probably just default to 60 or whatever your refresh rate is.
void LibretroWrapper::SetFullScreenRefreshRate()
{
    float refreshRateHz = std::abs(s_runtime_config["RefreshRate"].ValueAs<float>());

    if (refreshRateHz > 57.f && refreshRateHz < 58.f) {

        int display_in_use = 0; /* Only using first display */

        int display_mode_count = SDL_GetNumDisplayModes(display_in_use);
        if (display_mode_count < 1) {
            return;
        }

        for (int i = 0; i < display_mode_count; ++i) {

            SDL_DisplayMode mode;

            if (SDL_GetDisplayMode(display_in_use, i, &mode) != 0) {
                return;
            }

            if (SDL_BITSPERPIXEL(mode.format) >= 24 && mode.w == totalXRes && mode.h == totalYRes) {
                if (mode.refresh_rate == 57 || mode.refresh_rate == 58) {       // nvidia is fairly flexible in what refresh rate windows will show, so we can match either 57 or 58,
                    int result = SDL_SetWindowDisplayMode(s_window, &mode);     // both are totally non standard frequencies and shouldn't be set incorrectly
                    if (result == 0) {
                        printf("Custom fullscreen mode set: %ix%i@57.524 Hz\n", mode.w, mode.h);
                    }
                    break;
                }
            }
        }
    }
}

void LibretroWrapper::DestroyGLScreen()
{
  if (s_window != nullptr)
  {
    SDL_GL_DeleteContext(SDL_GL_GetCurrentContext());
    SDL_DestroyWindow(s_window);
  }
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
        << "Screenshot_"
        << std::setfill('0') << std::setw(4) << (1900 + ltm->tm_year)
        << '-'
        << std::setw(2) << (1 + ltm->tm_mon)
        << '-'
        << std::setw(2) << ltm->tm_mday
        << "_("
        << std::setw(2) << ltm->tm_hour
        << '-'
        << std::setw(2) << ltm->tm_min
        << '-'
        << std::setw(2) << ltm->tm_sec
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

LibretroWrapper::~LibretroWrapper() 
{
}

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
  SDL_GL_SwapWindow(g_ctx->getWindow());
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

  int xpos = s_runtime_config["WindowXPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED);
  int ypos = s_runtime_config["WindowYPosition"].ValueAsDefault<int>(SDL_WINDOWPOS_CENTERED);
  
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
    if (paused)
      Model3->RenderFrame();
    else
      Model3->RunFrame();

    // Check UI controls
    if (Inputs->uiExit->Pressed())
    {
      // Quit emulator
      quit = true;
    }
    else if (Inputs->uiReset->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
        SetAudioEnabled(false);
      }

      // Reset emulator
      Model3->Reset();

#ifdef SUPERMODEL_DEBUGGER
      // If debugger was supplied, reset it too
      if (Debugger != NULL)
        Debugger->Reset();
#endif // SUPERMODEL_DEBUGGER

      if (!paused)
      {
        Model3->ResumeThreads();
        SetAudioEnabled(true);
      }

      puts("Model 3 reset.");
    }
    else if (Inputs->uiPause->Pressed())
    {
      // Toggle emulator paused flag
      paused = !paused;

      if (paused)
      {
        Model3->PauseThreads();
        SetAudioEnabled(false);
        snprintf(titleStr, sizeof(titleStr), "%s (Paused)", baseTitleStr);
        SDL_SetWindowTitle(s_window, titleStr);
      }
      else
      {
        Model3->ResumeThreads();
        SetAudioEnabled(true);
        SDL_SetWindowTitle(s_window, baseTitleStr);
      }

      // Send paused value as output
      if (Outputs != NULL)
        Outputs->SetValue(OutputPause, paused);
    }
    else if (Inputs->uiSaveState->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
        SetAudioEnabled(false);
      }

      // Save game state
      SaveState(Model3);

      if (!paused)
      {
        Model3->ResumeThreads();
        SetAudioEnabled(true);
      }
    }
    else if (Inputs->uiChangeSlot->Pressed())
    {
      // Change save slot
      ++s_saveSlot;
      s_saveSlot %= 10; // clamp to [0,9]
      printf("Save slot: %d\n", s_saveSlot);
    }
    else if (Inputs->uiLoadState->Pressed())
    {
      if (!paused)
      {
        Model3->PauseThreads();
        SetAudioEnabled(false);
      }

      // Load game state
      LoadState(Model3);

#ifdef SUPERMODEL_DEBUGGER
      // If debugger was supplied, reset it after loading state
      if (Debugger != NULL)
        Debugger->Reset();
#endif // SUPERMODEL_DEBUGGER

      if (!paused)
      {
        Model3->ResumeThreads();
        SetAudioEnabled(true);
      }
    }
    else if (Inputs->uiMusicVolUp->Pressed())
    {
      // Increase music volume by 10%
      if (!Model3->GetGame().mpeg_board.empty())
      {
        int vol = (std::min)(200, s_runtime_config["MusicVolume"].ValueAs<int>() + 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
        printf("Music volume: %d%%", vol);
        if (200 == vol)
          puts(" (maximum)");
        else
          printf("\n");
      }
      else
        puts("This game does not have an MPEG music board.");
    }
    else if (Inputs->uiMusicVolDown->Pressed())
    {
      // Decrease music volume by 10%
      if (!Model3->GetGame().mpeg_board.empty())
      {
        int vol = (std::max)(0, s_runtime_config["MusicVolume"].ValueAs<int>() - 10);
        s_runtime_config.Get("MusicVolume").SetValue(vol);
        printf("Music volume: %d%%", vol);
        if (0 == vol)
          puts(" (muted)");
        else
          printf("\n");
      }
      else
        puts("This game does not have an MPEG music board.");
    }
    else if (Inputs->uiSoundVolUp->Pressed())
    {
      // Increase sound volume by 10%
    int vol = (std::min)(200, s_runtime_config["SoundVolume"].ValueAs<int>() + 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
      printf("Sound volume: %d%%", vol);
      if (200 == vol)
        puts(" (maximum)");
      else
        printf("\n");
    }
    else if (Inputs->uiSoundVolDown->Pressed())
    {
      // Decrease sound volume by 10%
      int vol = (std::max)(0, s_runtime_config["SoundVolume"].ValueAs<int>() - 10);
      s_runtime_config.Get("SoundVolume").SetValue(vol);
      printf("Sound volume: %d%%", vol);
      if (0 == vol)
        puts(" (muted)");
      else
        printf("\n");
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
      switch (crosshairs)
      {
      case 0: puts("Crosshairs disabled.");             break;
      case 3: puts("Crosshairs enabled.");              break;
      case 1: puts("Showing Player 1 crosshair only."); break;
      case 2: puts("Showing Player 2 crosshair only."); break;
      }
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
      printf("Frame limiting: %s\n", s_runtime_config["Throttle"].ValueAs<bool>() ? "On" : "Off");
    }
    else if (Inputs->uiScreenshot->Pressed())
    {
      // Make a screenshot
      Screenshot();
    }
#ifdef SUPERMODEL_DEBUGGER
      else if (Debugger != NULL && Inputs->uiEnterDebugger->Pressed())
      {
        // Break execution and enter debugger
        Debugger->ForceBreak(true);
      }
    }
#endif // SUPERMODEL_DEBUGGER

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

//  // Make sure all threads are paused before shutting down
//   Model3->PauseThreads();
//   // Save NVRAM
//   SaveNVRAM(Model3);

//   // Close audio
//   CloseAudio();

  // Shut down renderers
  // delete Render2D;
  // delete Render3D;
  // delete superAA;

  return 0;

  // Quit with an error
QuitError:
  // delete Render2D;
  // delete Render3D;
  // delete superAA;

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
    FILE* fp = fopen(s_configFilePath.c_str(), "r");
    if (fp)
    {
        fclose(fp);
        return;
    }

    // Write config
    fp = fopen(s_configFilePath.c_str(), "w");
    if (!fp)
    {
        ErrorLog("Unable to write default configuration file to %s", s_configFilePath.c_str());
        return;
    }
    fputs(s_defaultConfigFileContents, fp);
    fclose(fp);
    InfoLog("Wrote default configuration file to %s", s_configFilePath.c_str());
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
    SDL_SetWindowTitle(s_window, title.c_str());

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

Util::Config::Node DefaultConfig()
{
  Util::Config::Node config("Global");
  
  config.Set("GameXMLFile", s_gameXMLFilePath);
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
    config.Set("LogOutput", s_logFilePath.c_str());
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
        Util::Config::MergeINISections(&fConfig2, DefaultConfig(), fConfig1); 

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
        Util::Config::MergeINISections(&fileConfigWithDefaults, DefaultConfig(), fileConfig); 
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


// 1. Create the Hardware Implementation
    // (Ensure you have defined CLibretroInputSystem elsewhere!)
        m_inputSystem = std::make_shared<CLibretroInputSystem>();

    InputSystem = m_inputSystem;

    // 2. Create the CInputs Manager
    // We pass the hardware system to it.
    Inputs = new CInputs(m_inputSystem);
    // 3. Initialize the Inputs
    // This sets up the default mappings and prepares the system.
    if (!Inputs->Initialize())
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
    // DestroyGLScreen();
    // SDL_Quit();

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

