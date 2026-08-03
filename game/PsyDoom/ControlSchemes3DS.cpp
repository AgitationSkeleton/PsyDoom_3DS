//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom 3DS: named sets of control bindings. See 'ControlSchemes3DS.h'.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "ControlSchemes3DS.h"

#if PSYDOOM_3DS

#include "Config/Config.h"
#include "Controls.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

BEGIN_NAMESPACE(ControlSchemes3DS)

//------------------------------------------------------------------------------------------------------------------------------------------
// One binding within a scheme.
//
// Every scheme lists every binding it cares about, including the ones it deliberately leaves empty. A scheme that only
// listed what it wanted would inherit whatever the previous scheme had bound elsewhere, so switching between them
// would depend on the order they were switched in.
//------------------------------------------------------------------------------------------------------------------------------------------
struct SchemeBinding {
    Controls::Binding   binding;
    const char*         inputs;
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Scheme 1: the layout the Xbox 360 release of Doom II uses.
//
// The circle pad moves and strafes, the C-Stick turns, and the D-Pad is given over to weapon groups.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr SchemeBinding SCHEME_MODERN[] = {
    { Controls::Binding::Analog_MoveForward,    "Gamepad LeftY-"                    },
    { Controls::Binding::Analog_MoveBackward,   "Gamepad LeftY+"                    },
    { Controls::Binding::Analog_StrafeLeft,     "Gamepad LeftX-"                    },
    { Controls::Binding::Analog_StrafeRight,    "Gamepad LeftX+"                    },
    { Controls::Binding::Analog_TurnLeft,       "Gamepad RightX-"                   },
    { Controls::Binding::Analog_TurnRight,      "Gamepad RightX+"                   },
    { Controls::Binding::Digital_MoveForward,   ""                                  },
    { Controls::Binding::Digital_MoveBackward,  ""                                  },
    { Controls::Binding::Digital_StrafeLeft,    ""                                  },
    { Controls::Binding::Digital_StrafeRight,   ""                                  },
    { Controls::Binding::Digital_TurnLeft,      ""                                  },
    { Controls::Binding::Digital_TurnRight,     ""                                  },
    { Controls::Binding::Weapon_GroupShotguns,  "Gamepad DpUp"                      },
    { Controls::Binding::Weapon_GroupHeavy,     "Gamepad DpDown"                    },
    { Controls::Binding::Weapon_GroupRapid,     "Gamepad DpRight"                   },
    { Controls::Binding::Weapon_GroupEnergy,    "Gamepad DpLeft"                    },
    { Controls::Binding::Weapon_Previous,       "Gamepad LeftTrigger"               },
    { Controls::Binding::Weapon_Next,           "Gamepad RightTrigger"              },
    { Controls::Binding::Action_Use,            "Gamepad A, Gamepad B, Gamepad LeftShoulder" },
    { Controls::Binding::Action_Attack,         "Gamepad X, Gamepad RightShoulder"  },
    { Controls::Binding::Action_Respawn,        "Gamepad X, Gamepad RightShoulder"  },
    { Controls::Binding::Modifier_Run,          "Gamepad Y"                         },
    { Controls::Binding::Modifier_Strafe,       ""                                  },
    { Controls::Binding::Toggle_Pause,          "Gamepad Start"                     },
    { Controls::Binding::Menu_Back,             "Gamepad Back"                      },
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Scheme 2: the PlayStation layout.
//
// The D-Pad moves and turns as the original pad did, the shoulders strafe, and the face buttons keep their original
// jobs. The 3DS has two things the PlayStation pad did not, so they take the work the original had nowhere to put:
// the circle pad gets the weapon groups and the C-Stick gets analog turning.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr SchemeBinding SCHEME_PLAYSTATION[] = {
    { Controls::Binding::Analog_MoveForward,    ""                                  },
    { Controls::Binding::Analog_MoveBackward,   ""                                  },
    { Controls::Binding::Analog_StrafeLeft,     ""                                  },
    { Controls::Binding::Analog_StrafeRight,    ""                                  },
    { Controls::Binding::Analog_TurnLeft,       "Gamepad RightX-"                   },
    { Controls::Binding::Analog_TurnRight,      "Gamepad RightX+"                   },
    { Controls::Binding::Digital_MoveForward,   "Gamepad DpUp"                      },
    { Controls::Binding::Digital_MoveBackward,  "Gamepad DpDown"                    },
    { Controls::Binding::Digital_StrafeLeft,    "Gamepad LeftShoulder"              },
    { Controls::Binding::Digital_StrafeRight,   "Gamepad RightShoulder"             },
    { Controls::Binding::Digital_TurnLeft,      "Gamepad DpLeft"                    },
    { Controls::Binding::Digital_TurnRight,     "Gamepad DpRight"                   },
    { Controls::Binding::Weapon_GroupShotguns,  "Gamepad LeftY-"                    },
    { Controls::Binding::Weapon_GroupHeavy,     "Gamepad LeftY+"                    },
    { Controls::Binding::Weapon_GroupRapid,     "Gamepad LeftX+"                    },
    { Controls::Binding::Weapon_GroupEnergy,    "Gamepad LeftX-"                    },
    { Controls::Binding::Weapon_Previous,       "Gamepad LeftTrigger"               },
    { Controls::Binding::Weapon_Next,           "Gamepad RightTrigger"              },
    { Controls::Binding::Action_Use,            "Gamepad A"                         },
    { Controls::Binding::Action_Attack,         "Gamepad X"                         },
    { Controls::Binding::Action_Respawn,        "Gamepad X"                         },
    { Controls::Binding::Modifier_Run,          "Gamepad Y"                         },
    { Controls::Binding::Modifier_Strafe,       "Gamepad B"                         },
    { Controls::Binding::Toggle_Pause,          "Gamepad Start"                     },
    { Controls::Binding::Menu_Back,             "Gamepad Back"                      },
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Scheme 3: the PlayStation layout with use and strafe the other way round on B and A
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr SchemeBinding SCHEME_PLAYSTATION_ALT[] = {
    { Controls::Binding::Analog_MoveForward,    ""                                  },
    { Controls::Binding::Analog_MoveBackward,   ""                                  },
    { Controls::Binding::Analog_StrafeLeft,     ""                                  },
    { Controls::Binding::Analog_StrafeRight,    ""                                  },
    { Controls::Binding::Analog_TurnLeft,       "Gamepad RightX-"                   },
    { Controls::Binding::Analog_TurnRight,      "Gamepad RightX+"                   },
    { Controls::Binding::Digital_MoveForward,   "Gamepad DpUp"                      },
    { Controls::Binding::Digital_MoveBackward,  "Gamepad DpDown"                    },
    { Controls::Binding::Digital_StrafeLeft,    "Gamepad LeftShoulder"              },
    { Controls::Binding::Digital_StrafeRight,   "Gamepad RightShoulder"             },
    { Controls::Binding::Digital_TurnLeft,      "Gamepad DpLeft"                    },
    { Controls::Binding::Digital_TurnRight,     "Gamepad DpRight"                   },
    { Controls::Binding::Weapon_GroupShotguns,  "Gamepad LeftY-"                    },
    { Controls::Binding::Weapon_GroupHeavy,     "Gamepad LeftY+"                    },
    { Controls::Binding::Weapon_GroupRapid,     "Gamepad LeftX+"                    },
    { Controls::Binding::Weapon_GroupEnergy,    "Gamepad LeftX-"                    },
    { Controls::Binding::Weapon_Previous,       "Gamepad LeftTrigger"               },
    { Controls::Binding::Weapon_Next,           "Gamepad RightTrigger"              },
    { Controls::Binding::Action_Use,            "Gamepad B"                         },
    { Controls::Binding::Action_Attack,         "Gamepad X"                         },
    { Controls::Binding::Action_Respawn,        "Gamepad X"                         },
    { Controls::Binding::Modifier_Run,          "Gamepad Y"                         },
    { Controls::Binding::Modifier_Strafe,       "Gamepad A"                         },
    { Controls::Binding::Toggle_Pause,          "Gamepad Start"                     },
    { Controls::Binding::Menu_Back,             "Gamepad Back"                      },
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Looks up the binding list for a scheme. 'Custom' has none, and returns a count of zero.
//------------------------------------------------------------------------------------------------------------------------------------------
static const SchemeBinding* getSchemeBindings(const Scheme scheme, int32_t& numBindingsOut) noexcept {
    switch (scheme) {
        case Scheme::Modern:
            numBindingsOut = (int32_t) C_ARRAY_SIZE(SCHEME_MODERN);
            return SCHEME_MODERN;

        case Scheme::Playstation:
            numBindingsOut = (int32_t) C_ARRAY_SIZE(SCHEME_PLAYSTATION);
            return SCHEME_PLAYSTATION;

        case Scheme::PlaystationAlt:
            numBindingsOut = (int32_t) C_ARRAY_SIZE(SCHEME_PLAYSTATION_ALT);
            return SCHEME_PLAYSTATION_ALT;

        default:
            numBindingsOut = 0;
            return nullptr;
    }
}

const char* getSchemeName(const Scheme scheme) noexcept {
    switch (scheme) {
        case Scheme::Modern:            return "Modern";
        case Scheme::Playstation:       return "PSX";
        case Scheme::PlaystationAlt:    return "PSX Alt";
        default:                        return "Custom";
    }
}

void applyScheme(const Scheme scheme) noexcept {
    int32_t numBindings = 0;
    const SchemeBinding* const pBindings = getSchemeBindings(scheme, numBindings);

    if (!pBindings)
        return;

    for (int32_t i = 0; i < numBindings; ++i) {
        Controls::parseBinding(pBindings[i].binding, pBindings[i].inputs);
    }

    // The bindings file is written from whatever is in effect, so saying it needs saving is enough to make the file
    // follow the scheme. Without this the file would still describe the old scheme, and the next launch would read it
    // back, see it disagree, and conclude the player had edited it by hand.
    Config::gbNeedSave_Controls = true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Splits a binding into its individual inputs, tidied and sorted.
//
// Sorted because two bindings listing the same inputs in a different order are the same binding: pressing either of
// them does the same thing. Comparing the text as written would call them different and wrongly decide the player had
// edited something.
//------------------------------------------------------------------------------------------------------------------------------------------
static std::vector<std::string> splitInputs(const std::string& bindingStr) noexcept {
    std::vector<std::string> inputs;
    std::string current;

    for (const char c : bindingStr) {
        if (c == ',') {
            if (!current.empty()) {
                inputs.push_back(current);
                current.clear();
            }
        }
        else if ((c != ' ') || (!current.empty())) {
            current.push_back(c);
        }
    }

    if (!current.empty()) {
        inputs.push_back(current);
    }

    // Trailing spaces have to go too, since only leading ones were skipped above
    for (std::string& input : inputs) {
        while ((!input.empty()) && (input.back() == ' ')) {
            input.pop_back();
        }
    }

    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

bool bindingsMatchScheme(const Scheme scheme) noexcept {
    int32_t numBindings = 0;
    const SchemeBinding* const pBindings = getSchemeBindings(scheme, numBindings);

    // 'Custom' is whatever happens to be in effect, so it always matches
    if (!pBindings)
        return true;

    bool bMatches = true;

    for (int32_t i = 0; i < numBindings; ++i) {
        const Controls::Binding binding = pBindings[i].binding;

        // Compare like with like. The scheme spells its bindings the way a person would, the file may spell the same
        // thing differently, and only the binding code knows which spellings mean the same input - so put each through
        // it and compare what comes back out. Whatever was in effect is put back afterwards, since this is only ever
        // asked as a question and must not answer it by changing anything.
        std::string actual;
        Controls::bindingToStr(binding, actual);

        std::string wanted;
        Controls::parseBinding(binding, pBindings[i].inputs);
        Controls::bindingToStr(binding, wanted);

        Controls::parseBinding(binding, actual.c_str());

        if (splitInputs(actual) != splitInputs(wanted)) {
            bMatches = false;
            break;
        }
    }

    return bMatches;
}

Scheme resolveOnStartup(const Scheme savedScheme) noexcept {
    // Nothing to reconcile for a custom layout: the file is the layout
    if (savedScheme == Scheme::Custom)
        return Scheme::Custom;

    // The bindings have just been read from the file. If they no longer say what the scheme says, then the file has
    // been edited since the scheme was chosen, and those edits are what the player wants - so keep them and call the
    // selection what it now is, rather than overwriting their work on every launch.
    if (!bindingsMatchScheme(savedScheme))
        return Scheme::Custom;

    applyScheme(savedScheme);
    return savedScheme;
}

END_NAMESPACE(ControlSchemes3DS)

#endif  // #if PSYDOOM_3DS
