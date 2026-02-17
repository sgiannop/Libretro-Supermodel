#include <cstring>
#include <libretro.h>
#include "CLibretroInputSystem.h"
#include <Inputs/Input.h>

extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;

CLibretroInputSystem::CLibretroInputSystem() 
    : CInputSystem("Libretro")
{
    std::memset(m_joyButtons, 0, sizeof(m_joyButtons));
    std::memset(m_joyAxes,    0, sizeof(m_joyAxes));
    std::memset(m_joyPOV,     0, sizeof(m_joyPOV));
    std::memset(m_keyState,   0, sizeof(m_keyState));
}

CLibretroInputSystem::~CLibretroInputSystem() {}
bool CLibretroInputSystem::Poll()
{
    if (!input_poll_cb || !input_state_cb)
        return false;

    input_poll_cb();

    // ----- Keyboard -----
    for (int k = 0; k < 512; k++)
        m_keyState[k] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, k);

    const int16_t THRESHOLD = 8000;

    for (int joy = 0; joy < 2; joy++)
    {
        // ----- Initialize all buttons to 0 first -----
        for (int b = 0; b < NUM_JOY_BUTTONS; b++)
        {
            m_joyButtons[joy][b] = 0;
        }

        // ----- Axes -----
        int16_t x = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_X);

        int16_t y = input_state_cb(joy, RETRO_DEVICE_ANALOG,
                                   RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                   RETRO_DEVICE_ID_ANALOG_Y);

        m_joyAxes[joy][0] = x;
        m_joyAxes[joy][1] = y;

        // ----- D-Pad -----
        bool d_up    = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
        bool d_down  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
        bool d_left  = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
        bool d_right = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

        // Store in POV array (for IsJoyPOVInDir)
        m_joyPOV[joy][0] = (y < -THRESHOLD) || d_up;    // Up
        m_joyPOV[joy][1] = (x >  THRESHOLD) || d_right; // Right
        m_joyPOV[joy][2] = (y >  THRESHOLD) || d_down;  // Down
        m_joyPOV[joy][3] = (x < -THRESHOLD) || d_left;  // Left

        // ----- Buttons -----
        // Standard game buttons (0-3: B, A, Y, X)
        static const int game_buttons[4] =
        {
            RETRO_DEVICE_ID_JOYPAD_B,      // Button 0 (BUTTON1)
            RETRO_DEVICE_ID_JOYPAD_A,      // Button 1 (BUTTON2)
            RETRO_DEVICE_ID_JOYPAD_Y,      // Button 2 (BUTTON3)
            RETRO_DEVICE_ID_JOYPAD_X,      // Button 3 (BUTTON4)
        };

        for (int b = 0; b < 4; b++)
        {
            m_joyButtons[joy][b] =
                input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, game_buttons[b]);
        }

        // Gear shift / shoulder buttons (4-7: L, R, L2, R2)
        m_joyButtons[joy][4] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
        m_joyButtons[joy][5] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
        m_joyButtons[joy][6] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
        m_joyButtons[joy][7] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);

        // Start/Select buttons (8-9)
        m_joyButtons[joy][8] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
        m_joyButtons[joy][9] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
        
        // **FIX: Map D-Pad AND Analog Stick to buttons 10-13**
        // This makes both the analog stick and D-Pad work for movement!
        m_joyButtons[joy][10] = (y < -THRESHOLD) || d_up;      // Up - ANALOG + DIGITAL
        m_joyButtons[joy][11] = (y >  THRESHOLD) || d_down;    // Down - ANALOG + DIGITAL
        m_joyButtons[joy][12] = (x < -THRESHOLD) || d_left;    // Left - ANALOG + DIGITAL
        m_joyButtons[joy][13] = (x >  THRESHOLD) || d_right;   // Right - ANALOG + DIGITAL

        // Service/Test buttons (20-23) - BUTTON21-24 in Supermodel numbering
        m_joyButtons[joy][20] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
        m_joyButtons[joy][21] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
        m_joyButtons[joy][22] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
        m_joyButtons[joy][23] = input_state_cb(joy, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
    }

    return true;
}

bool CLibretroInputSystem::IsJoyButPressed(int joyNum, int butNum) const
{
    if (joyNum < 0 || joyNum >= 2) return false;
    if (butNum < 0 || butNum >= NUM_JOY_BUTTONS) return false;

    return m_joyButtons[joyNum][butNum] != 0;
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
    if (povDir < 0 || povDir > 3) return false;

    return m_joyPOV[joyNum][povDir] != 0;
}


int CLibretroInputSystem::GetKeyIndex(const char *keyName)
{
    if (!keyName) return -1;
    
    // Map Supermodel KEY_* to libretro RETROK_* codes
    if (std::strcmp(keyName, "KEY_1") == 0) return RETROK_1;
    if (std::strcmp(keyName, "KEY_2") == 0) return RETROK_2;
    if (std::strcmp(keyName, "KEY_3") == 0) return RETROK_3;
    if (std::strcmp(keyName, "KEY_4") == 0) return RETROK_4;
    if (std::strcmp(keyName, "KEY_5") == 0) return RETROK_5;  // Service A
    if (std::strcmp(keyName, "KEY_6") == 0) return RETROK_6;  // Test A
    if (std::strcmp(keyName, "KEY_7") == 0) return RETROK_7;  // Service B
    if (std::strcmp(keyName, "KEY_8") == 0) return RETROK_8;  // Test B
    if (std::strcmp(keyName, "KEY_UP") == 0) return RETROK_UP;
    if (std::strcmp(keyName, "KEY_DOWN") == 0) return RETROK_DOWN;
    if (std::strcmp(keyName, "KEY_LEFT") == 0) return RETROK_LEFT;
    if (std::strcmp(keyName, "KEY_RIGHT") == 0) return RETROK_RIGHT;
    if (std::strcmp(keyName, "KEY_A") == 0) return RETROK_a;
    if (std::strcmp(keyName, "KEY_S") == 0) return RETROK_s;
    if (std::strcmp(keyName, "KEY_D") == 0) return RETROK_d;
    if (std::strcmp(keyName, "KEY_F") == 0) return RETROK_f;
    if (std::strcmp(keyName, "KEY_Q") == 0) return RETROK_q;
    if (std::strcmp(keyName, "KEY_W") == 0) return RETROK_w;
    if (std::strcmp(keyName, "KEY_E") == 0) return RETROK_e;
    if (std::strcmp(keyName, "KEY_R") == 0) return RETROK_r;
    
    return -1;
}

bool CLibretroInputSystem::IsKeyPressed(int kbdNum, int keyIndex) const
{
    if (kbdNum != 0) return false;

    if (keyIndex >= 0 && keyIndex < 512)
        return m_keyState[keyIndex];

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

const JoyDetails *CLibretroInputSystem::GetJoyDetails(int joyNum)
{
    static JoyDetails d;
    static bool initialized = false;

    if (!initialized) {
        std::memset(&d, 0, sizeof(d));
        std::strncpy(d.name, "Libretro Joypad", MAX_NAME_LENGTH);
        
        d.numButtons = 32; 
        d.numAxes = 2;
        d.numPOVs = 1; 

        d.hasAxis[0] = true; // X
        d.hasAxis[1] = true; // Y
        initialized = true;
    }
    return &d;
}

bool CLibretroInputSystem::InitializeSystem() { return true; }
int CLibretroInputSystem::GetMouseWheelDir(int mseNum) const { return 0; }
const char *CLibretroInputSystem::GetKeyName(int keyIndex) { return "NONE"; }