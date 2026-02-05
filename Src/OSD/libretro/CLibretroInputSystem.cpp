#include <cstring>
#include <libretro.h>
#include "CLibretroInputSystem.h"
#include <Inputs/Input.h>
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;

CLibretroInputSystem::CLibretroInputSystem() : CInputSystem("Libretro")
{
    std::memset(m_joyState, 0, sizeof(m_joyState));
    std::memset(m_joyAxes, 0, sizeof(m_joyAxes));
    //m_numJoys = 2; 
}



void CLibretroInputSystem::DebugPrintDPad() {
    if (!input_poll_cb || !input_state_cb)
        return;

    // Poll the inputs
    input_poll_cb();

    const int DEADZONE = 8000;

    for (int joy = 0; joy < 2; joy++) // <-- joy is defined here
    {
        // Read analog stick X/Y
        int16_t x = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X);
        int16_t y = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_Y);

        // Read digital D-PAD
        bool up    = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        bool down  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        bool left  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        bool right = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

        // Print out values
        printf("JOY%d: X=%d Y=%d UP=%d DOWN=%d LEFT=%d RIGHT=%d\n",
               joy, x, y, up, down, left, right);

        // Optionally map analog stick to D-PAD buttons
        bool analog_up    = y < -DEADZONE;
        bool analog_down  = y >  DEADZONE;
        bool analog_left  = x < -DEADZONE;
        bool analog_right = x >  DEADZONE;

        
    }
}
CLibretroInputSystem::~CLibretroInputSystem() {}
bool CLibretroInputSystem::Poll()
{
    if (!input_poll_cb || !input_state_cb) return false;
    input_poll_cb();

    const int16_t THRESHOLD = 8000;

    for (int joy = 0; joy < 2; joy++)
    {
        // 1. Read Analog Stick Positions
        int16_t x = input_state_cb(joy, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        int16_t y = input_state_cb(joy, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        
        m_joyAxes[joy][0] = x;
        m_joyAxes[joy][1] = y;

        // 2. Read Digital D-Pad State
        bool d_up    = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        bool d_down  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        bool d_left  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        bool d_right = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

        // 3. Combine them into your m_joyState (Indices 10-13)
        // This makes the D-Pad and Stick interchangeable
        m_joyState[joy][10] = (y < -THRESHOLD) || d_up;    // Up
        m_joyState[joy][11] = (y >  THRESHOLD) || d_down;  // Down
        m_joyState[joy][12] = (x < -THRESHOLD) || d_left;  // Left
        m_joyState[joy][13] = (x >  THRESHOLD) || d_right; // Right

        // 4. Buttons (B, A, Y, X, etc.)
        static const int libretro_buttons[] = {
            RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y,
            RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
            RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
            RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START
        };

        for (int b = 0; b < 10; b++) {
            m_joyState[joy][b] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, libretro_buttons[b]);
        }

        if (input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP)) {
            printf("DEBUG: LIBRETRO REPORTS UP PRESSED FOR JOY %d\n", joy);
        }
        if (input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN)) {
            printf("DEBUG: LIBRETRO REPORTS DOWN PRESSED FOR JOY %d\n", joy);
        }
    }

    
    return true;
}
bool CLibretroInputSystem::IsJoyButPressed(int joyNum, int butNum) const
{
    if (joyNum < 0 || joyNum >= 2) return false;
    if (butNum < 0 || butNum >= 14) return false;  // <- allow D-PAD
    return m_joyState[joyNum][butNum] != 0;
}

int CLibretroInputSystem::GetJoyAxisValue(int joyNum, int axisNum) const
{
    if (joyNum < 0 || joyNum >= 2) return 0;
    if (axisNum < 0 || axisNum >= 2) return 0;
    return m_joyAxes[joyNum][axisNum];
}

bool CLibretroInputSystem::IsJoyPOVInDir(int joyNum, int povNum, int povDir) const
{
    if (joyNum < 0 || joyNum >= 2) return false;

    // povDir mapping for Supermodel: 0=Up, 1=Right, 2=Down, 3=Left
    switch (povDir)
    {
        case 0: return m_joyState[joyNum][10] != 0; // UP
        case 1: return m_joyState[joyNum][13] != 0; // RIGHT
        case 2: return m_joyState[joyNum][11] != 0; // DOWN
        case 3: return m_joyState[joyNum][12] != 0; // LEFT
        default: return false;
    }
}
int CLibretroInputSystem::GetKeyIndex(const char *keyName)
{
    if (!keyName) return -1;
    if (std::strcmp(keyName, "KEY_1") == 0) return 30;
    if (std::strcmp(keyName, "KEY_3") == 0) return 32;
    if (std::strcmp(keyName, "KEY_5") == 0) return 34;
    if (std::strcmp(keyName, "KEY_UP") == 0) return 72;
    if (std::strcmp(keyName, "KEY_DOWN") == 0) return 80;
    if (std::strcmp(keyName, "KEY_LEFT") == 0) return 75;
    if (std::strcmp(keyName, "KEY_RIGHT") == 0) return 77;
    return -1;
}

bool CLibretroInputSystem::IsKeyPressed(int kbdNum, int keyIndex) const
{
    switch(keyIndex) {
        case 30: return m_joyState[0][9] != 0;  // Start
        case 32: return m_joyState[0][8] != 0;  // Select
        case 72: return m_joyState[0][10] != 0; // Up
        case 80: return m_joyState[0][11] != 0; // Down
        case 75: return m_joyState[0][12] != 0; // Left
        case 77: return m_joyState[0][13] != 0; // Right
    }
    return false;
}

bool CLibretroInputSystem::IsMouseButPressed(int mseNum, int butNum) const { return false; }
int CLibretroInputSystem::GetMouseAxisValue(int mseNum, int axisNum) const { return 0; }
void CLibretroInputSystem::SetMouseVisibility(bool visible) {}
const MouseDetails *CLibretroInputSystem::GetMouseDetails(int mseNum) { return nullptr; }
bool CLibretroInputSystem::ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd) { return false; }
const KeyDetails *CLibretroInputSystem::GetKeyDetails(int kbdNum)
{
    static KeyDetails d{};
    std::strncpy(d.name, "Libretro Keyboard", MAX_NAME_LENGTH);
    return &d;
}
// In your constructor or InitializeSystem:

const JoyDetails *CLibretroInputSystem::GetJoyDetails(int joyNum)
{
    static JoyDetails d;
    static bool initialized = false;

    if (!initialized) {
        std::memset(&d, 0, sizeof(d));
        std::strncpy(d.name, "Libretro Joypad", MAX_NAME_LENGTH);
        
        // This MUST be > 0 before CInput::CreateSource() is called
        d.numButtons = 14; 
        d.numAxes = 2;
        d.numPOVs = 1; 

        // Supermodel checks these flags for Axis mappings
        d.hasAxis[0] = true; // X
        d.hasAxis[1] = true; // Y
        initialized = true;
    }
    return &d;
}
bool CLibretroInputSystem::InitializeSystem() { return true; }
int CLibretroInputSystem::GetMouseWheelDir(int mseNum) const { return 0; }
const char *CLibretroInputSystem::GetKeyName(int keyIndex)
{
    return "NONE";
}

