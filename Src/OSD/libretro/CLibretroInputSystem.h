#ifndef LIBRETRO_INPUT_SYSTEM_H
#define LIBRETRO_INPUT_SYSTEM_H

#include <Inputs/InputSystem.h>
#include "libretro.h"

class CLibretroInputSystem : public CInputSystem
{
public:
    CLibretroInputSystem();
    virtual ~CLibretroInputSystem() override;

    // Must match EXACTLY the =0 functions in InputSystem.h
    virtual bool InitializeSystem() override;
    virtual bool Poll() override;
    void DebugPrintDPad();

    virtual int GetNumKeyboards() const override { return 1; }
    virtual int GetKeyIndex(const char *keyName) override;
    virtual const char *GetKeyName(int keyIndex) override;
    virtual bool IsKeyPressed(int kbdNum, int keyIndex) const override;
    virtual const KeyDetails *GetKeyDetails(int kbdNum) override;

    virtual int GetNumMice() const override { return 1; }
    virtual int GetMouseAxisValue(int mseNum, int axisNum) const override;
    virtual int GetMouseWheelDir(int mseNum) const override;
    virtual bool IsMouseButPressed(int mseNum, int butNum) const override;
    virtual const MouseDetails *GetMouseDetails(int mseNum) override;
    virtual void SetMouseVisibility(bool visible) override;

    virtual int GetNumJoysticks() const override { return 2; }
    virtual int GetJoyAxisValue(int joyNum, int axisNum) const override;
    virtual bool IsJoyPOVInDir(int joyNum, int povNum, int povDir) const override;
    virtual bool IsJoyButPressed(int joyNum, int butNum) const override;
    virtual const JoyDetails *GetJoyDetails(int joyNum) override;

    // Signature match check: Supermodel usually uses the struct directly, 
    // but ensure your .cpp matches this exactly.
    virtual bool ProcessForceFeedbackCmd(int joyNum, int axisNum, ForceFeedbackCmd ffCmd) override;
    std::shared_ptr<CInputSource> ParseSource(const char* mapping, bool isAxis);

private:
    int16_t m_joyState[2][NUM_JOY_BUTTONS];
    int16_t m_joyAxes[2][NUM_JOY_AXES];
    uint8_t m_joyPOV[2][4]; // Up, Right, Down, Left]


};

#endif