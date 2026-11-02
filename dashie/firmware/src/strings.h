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
  S_SET, S_CHANGE, S_PRESSCANCEL, S_BT_IDLE_BTN,   // button-only boards (Plus2)
  // shift mode
  S_SHIFT, S_START, S_READ, S_NOTE, S_END, S_SH_NONE, S_SH_HOLDA, S_SH_HINT_CORE, S_SH_HINT_STICK, S_SH_NOTES, S_SH_QUEUE, S_SH_BATT, S_SH_STORE, S_SH_LAST, S_SH_READY, S_SH_NOLAST, S_SH_OFFLINE, S_SH_LOWSTORE, S_SH_WRITEERR, S_SH_LOWBATT, S_SH_SUMREADY, S_REC_NOTE, S_ENDQ, S_ENDQ_CORE, S_ENDQ_STICK, S_Q1, S_Q2, S_Q3, S_Q4, S_Q_HINT_CORE, S_Q_HINT_STICK, S_SENDING, S_SUMMARIZING, S_SAVED_OFFLINE, S_ERR_NOSTORE, S_NO_SUMMARY,
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
  /* S_SET */ {"SET", "הגדר"},
  /* S_CHANGE */ {"CHANGE", "שנה"},
  /* S_PRESSCANCEL */ {"Press A to cancel", "לחץ A לביטול"},
  /* S_BT_IDLE_BTN */ {"Press B", "לחץ B"},
  /* S_SHIFT */ {"Shift", "משמרת"},
  /* S_START */ {"START", "התחל"},
  /* S_READ */ {"READ", "הקרא"},
  /* S_NOTE */ {"NOTE", "הערה"},
  /* S_END */ {"END", "סיים"},
  /* S_SH_NONE */ {"No shift running", "אין משמרת פעילה"},
  /* S_SH_HOLDA */ {"Hold A to start", "החזק A להתחלה"},
  /* S_SH_HINT_CORE */ {"Side btn: note, hold: end", "צד: הערה, החזק: סיום"},
  /* S_SH_HINT_STICK */ {"A: note, hold A: end", "A: הערה, החזק A: סיום"},
  /* S_SH_NOTES */ {"Notes", "הערות"},
  /* S_SH_QUEUE */ {"To upload", "להעלאה"},
  /* S_SH_BATT */ {"Battery", "סוללה"},
  /* S_SH_STORE */ {"Storage", "אחסון"},
  /* S_SH_LAST */ {"Last summary", "סיכום אחרון"},
  /* S_SH_READY */ {"ready", "מוכן"},
  /* S_SH_NOLAST */ {"none", "אין"},
  /* S_SH_OFFLINE */ {"offline", "לא מחובר"},
  /* S_SH_LOWSTORE */ {"Storage low!", "האחסון כמעט מלא!"},
  /* S_SH_WRITEERR */ {"Write error!", "שגיאת כתיבה!"},
  /* S_SH_LOWBATT */ {"Battery low!", "סוללה חלשה!"},
  /* S_SH_SUMREADY */ {"Summary ready: READ", "סיכום מוכן: הקרא"},
  /* S_REC_NOTE */ {"Note", "הערה"},
  /* S_ENDQ */ {"End shift?", "לסיים משמרת?"},
  /* S_ENDQ_CORE */ {"Click the side button", "לחץ על הכפתור הצדדי"},
  /* S_ENDQ_STICK */ {"Click A to confirm", "לחץ A לאישור"},
  /* S_Q1 */ {"How did the line run?", "איך עבר הקו?"},
  /* S_Q2 */ {"Any incidents?", "היו אירועים חריגים?"},
  /* S_Q3 */ {"How was each station?", "איך כל עמדה?"},
  /* S_Q4 */ {"How did I handle things?", "איך התנהלתי?"},
  /* S_Q_HINT_CORE */ {"Side btn: answer, hold: skip", "צד: ענה, החזק: דלג"},
  /* S_Q_HINT_STICK */ {"A: answer, hold A: skip", "A: ענה, החזק A: דלג"},
  /* S_SENDING */ {"Sending notes...", "שולח הערות..."},
  /* S_SUMMARIZING */ {"Summarizing...", "מכין סיכום..."},
  /* S_SAVED_OFFLINE */ {"Saved. Sends when online", "נשמר. יישלח כשיש רשת"},
  /* S_ERR_NOSTORE */ {"No storage", "אין אחסון"},
  /* S_NO_SUMMARY */ {"No summary yet", "אין סיכום עדיין"},
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
