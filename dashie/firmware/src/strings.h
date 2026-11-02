#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// All on-device UI strings live here (EN + HE). Add a row to the enum AND the table.
// Hebrew needs a font: see README ("Hebrew font"). Without src/font_he.h the build
// still works and HE silently falls back to EN (HAVE_HE_FONT undefined).
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include("font_he.h")
#    define HAVE_HE_FONT 1
#  endif
#endif

enum StrId : uint8_t {
  S_TALK, S_NEXT, S_OPT, S_RESET,
  S_LISTENING, S_RECORDING, S_THINKING,
  S_ERR_NOWIFI, S_ERR_NOBRIDGE, S_ERR_BRIDGE, S_ERR_MEM, S_ERR_NOCATCH, S_ERR_BUSY, S_NOREPLY, S_NONLATIN,
  S_IMPACT, S_AREYOUOK, S_TAPCANCEL, S_SOS,
  S_NOISE, S_GFORCE, S_TRIP, S_SETTINGS,
  S_MAXG, S_BRAKES, S_TURNS, S_TIME, S_AVGNOISE, S_BESTQUIET, S_QUIET, S_WHOA, S_PRESS_AGAIN,
  S_BRIGHT, S_VOLUME, S_SENS, S_THEME, S_VOICE, S_LANG,
  S_LOW, S_MED, S_HIGH, S_ON, S_OFF, S_CYAN, S_AMBER, S_GREEN,
  S_LANG_EN, S_LANG_HE, S_LANG_HE_NOFONT,
  S_TESTKIT, S_BT_IDLE, S_BT_CONN, S_BT_OK, S_BT_NF, S_BT_TO, S_BT_MEM, S_BT_FAIL, S_BT_PICK, S_BT_NODEV, S_BACK, S_PICK,
  S_COUNT
};

struct StrRow { const char* en; const char* he; };

static const StrRow kStrings[S_COUNT] = {
  /* S_TALK */ {"TALK", "דבר"},
  /* S_NEXT */ {"NEXT", "הבא"},
  /* S_OPT */ {"OPT", "שנה"},
  /* S_RESET */ {"RESET", "אפס"},
  /* S_LISTENING */ {"Listening...", "מקשיב..."},
  /* S_RECORDING */ {"Recording", "מקליט"},
  /* S_THINKING */ {"Thinking...", "חושב..."},
  /* S_ERR_NOWIFI */ {"No Wi-Fi", "אין Wi-Fi"},
  /* S_ERR_NOBRIDGE */ {"No bridge", "אין שרת"},
  /* S_ERR_BRIDGE */ {"Bridge error", "שגיאת שרת"},
  /* S_ERR_MEM */ {"Out of memory", "אין זיכרון"},
  /* S_ERR_NOCATCH */ {"Didn't catch that", "לא שמעתי"},
  /* S_ERR_BUSY */ {"Busy, try again", "עסוק, נסה שוב"},
  /* S_NOREPLY */ {"(no reply)", "(אין תשובה)"},
  /* S_NONLATIN */ {"(reply needs HE font)", "(התשובה דורשת פונט)"},
  /* S_IMPACT */ {"IMPACT DETECTED", "זוהתה פגיעה!"},
  /* S_AREYOUOK */ {"Are you OK?", "הכול בסדר?"},
  /* S_TAPCANCEL */ {"Tap to cancel", "לחץ לביטול"},
  /* S_SOS */ {"SOS - call emergency", "קרא לעזרה!"},
  /* S_NOISE */ {"Cabin noise", "רעש בתא"},
  /* S_GFORCE */ {"G-Force", "כוחות G"},
  /* S_TRIP */ {"Trip", "נסיעה"},
  /* S_SETTINGS */ {"Settings", "הגדרות"},
  /* S_MAXG */ {"Max G", "G מרבי"},
  /* S_BRAKES */ {"Hard brakes", "בלימות חדות"},
  /* S_TURNS */ {"Sharp turns", "פניות חדות"},
  /* S_TIME */ {"Time", "זמן"},
  /* S_AVGNOISE */ {"Avg noise", "רעש ממוצע"},
  /* S_BESTQUIET */ {"Best quiet", "שיא שקט"},
  /* S_QUIET */ {"Quiet", "שקט"},
  /* S_WHOA */ {"Whoa!", "וואו!"},
  /* S_PRESS_AGAIN */ {"Press again to reset", "לחץ שוב לאיפוס"},
  /* S_BRIGHT */ {"Brightness", "בהירות"},
  /* S_VOLUME */ {"Volume", "עוצמה"},
  /* S_SENS */ {"Voice sensitivity", "רגישות קול"},
  /* S_THEME */ {"Theme", "ערכת צבעים"},
  /* S_VOICE */ {"Voice trigger", "הפעלה בקול"},
  /* S_LANG */ {"Language", "שפה"},
  /* S_LOW */ {"Low", "נמוכה"},
  /* S_MED */ {"Medium", "בינונית"},
  /* S_HIGH */ {"High", "גבוהה"},
  /* S_ON */ {"On", "פעיל"},
  /* S_OFF */ {"Off", "כבוי"},
  /* S_CYAN */ {"Cyan", "ציאן"},
  /* S_AMBER */ {"Amber", "ענבר"},
  /* S_GREEN */ {"Green", "ירוק"},
  /* S_LANG_EN */ {"English", "English"},
  /* S_LANG_HE */ {"עברית", "עברית"},
  /* S_LANG_HE_NOFONT */ {"Hebrew (no font)", "Hebrew (no font)"},
  /* S_TESTKIT */ {"Test kit", "בדיקת דיבורית"},
  /* S_BT_IDLE */ {"Press OPT", "לחץ שנה"},
  /* S_BT_CONN */ {"Connecting...", "מתחבר..."},
  /* S_BT_OK */ {"Connected", "מחובר"},
  /* S_BT_NF */ {"Not found", "לא נמצאה"},
  /* S_BT_TO */ {"Timeout", "תם הזמן"},
  /* S_BT_MEM */ {"Low memory", "אין זיכרון"},
  /* S_BT_FAIL */ {"Failed", "נכשל"},
  /* S_BT_PICK */ {"Pick your kit", "בחר דיבורית"},
  /* S_BT_NODEV */ {"None found. Pairing mode?", "לא נמצא. מצב צימוד?"},
  /* S_BACK */ {"BACK", "חזור"},
  /* S_PICK */ {"PICK", "בחר"},
};

enum Lang : uint8_t { LANG_EN = 0, LANG_HE = 1 };
static uint8_t g_lang = LANG_EN;   // persisted in Preferences ("lang")

// True only if Hebrew is selected AND a Hebrew font was compiled in.
inline bool heActive() {
#ifdef HAVE_HE_FONT
  return g_lang == LANG_HE;
#else
  return false;
#endif
}

inline const char* ui(StrId id) { return heActive() ? kStrings[id].he : kStrings[id].en; }
