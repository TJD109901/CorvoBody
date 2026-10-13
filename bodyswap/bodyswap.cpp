// Body Swap - Dishonored Mod Loader plugin for CorvoBody.
// Adds "Choose Body" to Pause > Mods: pick Corvo, Daud or the Overseer disguise for the first-person body; the
// first-person arms follow the body (user 2026-10-11: the separate Arms row is no longer needed - the body draws the
// high-quality arms itself).
// The loader remembers the choices and calls SetValue with them at startup; each one is forwarded to CorvoBody
// (dinput8.dll next to Dishonored.exe) through its CorvoBody_SetCharacter export.
//
// Plugin API (matches the Better Powers plugins): cdecl exports Plg_Init / Plg_Query / Plg_QueryMenu /
// Plg_SetEnabled; all menu text is UTF-16.
#include <windows.h>
#include <string.h>

namespace {

struct PluginInfo {
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* title;
    int version;
};

struct MenuItem {               // 0x24 bytes
    int type;                   // 1 = choice list, 2 = read-only status line
    const wchar_t* label;
    int choiceCount;
    const wchar_t* const* choices;
    int reserved0;
    int reserved1;
    int flag;                   // 1 in every item of the reference plugins
    int reserved2;
    int group;                  // index into groupNames, -1 = none
};
static_assert(sizeof(MenuItem) == 0x24, "menu item layout");

using GetValueFn = int(__cdecl*)(int);
using SetValueFn = void(__cdecl*)(int, int);

struct MenuDesc {
    int itemStride;             // sizeof(MenuItem)
    int unknown1;               // 3
    int unknown2;               // 1
    int itemCount;
    MenuItem* items;
    GetValueFn getValue;
    SetValueFn setValue;
    int groupCount;
    const wchar_t* const* groupNames;
    int reserved;
};

using SetCharacterFn = void(__cdecl*)(int part, int value);
using GetCharacterFn = int(__cdecl*)(int part);

const wchar_t* const kStatusEn[] = { L"CorvoBody not found", L"Active" };
const wchar_t* const kStatusIt[] = { L"CorvoBody non trovato", L"Attivo" };
const wchar_t* const kChars[] = { L"Corvo", L"Daud" };
// Body / arms / blade also offer Daud's High Overseer disguise (Brigmore Witches, Coldridge Prison).
const wchar_t* const kChars3En[] = { L"Corvo", L"Daud", L"Overseer" };
const wchar_t* const kChars3It[] = { L"Corvo", L"Daud", L"Supervisore" };
// Summoned NPC (Summon Assassin power, 3.1): the regular whaler, Corvo's / Daud's player model, or an Overseer hound ("Doggy!").
const wchar_t* const kSummonEn[] = { L"Assassin", L"Corvo", L"Daud", L"Doggy!" };
const wchar_t* const kSummonIt[] = { L"Assassino", L"Corvo", L"Daud", L"Cagnolino!" };
const wchar_t* const kGroupsEn[] = { L"Character" };
const wchar_t* const kGroupsIt[] = { L"Personaggio" };

PluginInfo g_info = { L"Body Swap", L"Choose Corvo's, Daud's or the Overseer disguise's body (with its arms) and blade, Corvo's or Daud's powers, crossbow and jump, and who Summon Assassin brings (needs CorvoBody)", L"Choose Body", 1 };

MenuItem g_items[7] = {
    { 2, L"Status", 2, kStatusEn, 0, 0, 1, 0, -1 },
    { 1, L"Body",   3, kChars3En, 0, 0, 1, 0,  0 },
    { 1, L"Blade",  3, kChars3En, 0, 0, 1, 0,  0 },
    { 1, L"Powers", 2, kChars,    0, 0, 1, 0,  0 },
    { 1, L"Crossbow", 2, kChars,  0, 0, 1, 0,  0 },
    { 1, L"Jump", 2, kChars,      0, 0, 1, 0,  0 },   // Agility: Corvo's double jump / Daud's one continuous jump
    { 1, L"Summoned NPC", 4, kSummonEn, 0, 0, 1, 0, 0 },   // Summon Assassin: who comes (assassin / Corvo / Daud / an Overseer hound)
};

// Menu rows: [1] body, [2] blade, [3] power set, [4] crossbow, [5] jump (0 Corvo, 1 Daud, 2 Overseer for 1-2),
// [6] summoned NPC (0 assassin, 1 Corvo, 2 Daud, 3 Overseer hound).
// CorvoBody parts: 0 body, 1 arms (= body), 2 blade, 3 powers, 4 crossbow, 5 jump, 6 summoned assassins.
volatile LONG g_value[7] = { 0, 0, 0, 0, 0, 0, 0 };
const int kPartOfRow[7] = { -1, 0, 2, 3, 4, 5, 6 };
const int kRows = 7;

int MaxValue(int idx) { return idx == 6 ? 3 : (idx >= 1 && idx <= 2) ? 2 : 1; }
volatile LONG g_enabled = 1;
bool g_italian = false;

SetCharacterFn g_set = nullptr;
GetCharacterFn g_get = nullptr;

// CorvoBody is the dinput8.dll next to the exe. Ask for it by full path: the Mod Loader's own proxy loads the
// system dinput8.dll as well, so the bare name can match either.
bool ResolveCorvoBody() {
    if (g_set) return true;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    HMODULE mod = nullptr;
    if (n && n < MAX_PATH) {
        char* slash = strrchr(path, '\\');
        if (slash && (slash - path) + 12 < MAX_PATH) { strcpy(slash + 1, "dinput8.dll"); mod = GetModuleHandleA(path); }
    }
    if (!mod) mod = GetModuleHandleA("dinput8.dll");
    if (!mod) return false;
    g_set = reinterpret_cast<SetCharacterFn>(GetProcAddress(mod, "CorvoBody_SetCharacter"));
    g_get = reinterpret_cast<GetCharacterFn>(GetProcAddress(mod, "CorvoBody_GetCharacter"));
    return g_set != nullptr;
}

void Push() {
    if (!ResolveCorvoBody()) return;
    bool on = g_enabled != 0;
    g_set(0, on ? g_value[1] : 0);
    g_set(1, on ? g_value[1] : 0);   // the first-person arms follow the body
    g_set(2, on ? g_value[2] : 0);
    g_set(3, on ? g_value[3] : 0);
    g_set(4, on ? g_value[4] : 0);
    g_set(5, on ? g_value[5] : 0);
    g_set(6, on ? g_value[6] : 0);   // (an older CorvoBody ignores part 6)
}

int __cdecl GetValue(int idx) {
    if (idx == 0) return ResolveCorvoBody() ? 1 : 0;
    if (idx >= 1 && idx < kRows) return g_value[idx];
    return 0;
}

void __cdecl SetValue(int idx, int value) {
    if (idx < 1 || idx >= kRows) return;
    if (value < 0 || value > MaxValue(idx)) return;
    InterlockedExchange(&g_value[idx], value);
    Push();
}

MenuDesc g_menu = { sizeof(MenuItem), 3, 1, kRows, g_items, &GetValue, &SetValue, 1, kGroupsEn, 0 };

} // namespace

extern "C" {

__declspec(dllexport) void __cdecl Plg_Init(const char* lang) {
    g_italian = lang && lang[0] == 'I' && lang[1] == 'T' && lang[2] == 'A';
    if (g_italian) {
        g_info.description = L"Scegli corpo (con le sue braccia) e lama di Corvo, di Daud o del travestimento da Supervisore, poteri, balestra e salto di Corvo o di Daud, e chi arriva con Evoca assassino (richiede CorvoBody)";
        g_info.title = L"Scegli corpo";
        g_items[0].label = L"Stato";
        g_items[0].choices = kStatusIt;
        g_items[1].label = L"Corpo";
        g_items[2].label = L"Lama";
        g_items[3].label = L"Poteri";
        g_items[4].label = L"Balestra";
        g_items[5].label = L"Salto";
        g_items[6].label = L"PNG evocati";
        g_items[6].choices = kSummonIt;
        for (int i = 1; i <= 2; ++i) g_items[i].choices = kChars3It;
        g_menu.groupNames = kGroupsIt;
    }
    // Start from what CorvoBody already uses (its ini); the loader then applies the saved menu values.
    if (ResolveCorvoBody() && g_get) {
        for (int i = 1; i < kRows; ++i) {
            int v = g_get(kPartOfRow[i]);
            g_value[i] = (v >= 0 && v <= MaxValue(i)) ? v : 0;
        }
    }
}

__declspec(dllexport) PluginInfo* __cdecl Plg_Query() { return &g_info; }

__declspec(dllexport) MenuDesc* __cdecl Plg_QueryMenu() { return &g_menu; }

__declspec(dllexport) void __cdecl Plg_SetEnabled(int enabled) {
    InterlockedExchange(&g_enabled, enabled ? 1 : 0);
    Push();   // disabled: back to Corvo
}

}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
