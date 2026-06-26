/*
 * BCDToolWindows – EFI Boot Manager per Windows
 *
 * Requisiti di sistema:
 *   - Windows 8 / Windows Server 2012 o superiore (supporto UEFI in Windows)
 *   - Firmware UEFI (non Legacy BIOS)
 *   - Eseguire come Amministratore
 *
 * Compilazione: vedi CMakeLists.txt o build.bat
 */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fcntl.h>    // _O_U8TEXT
#include <io.h>       // _setmode, _fileno
#include <string>
#include <vector>
#include <algorithm>
#include "compat.h"   // ReadWideLine, _wcsicmp, ScanWideTwo

#include "privilege.h"
#include "gpt_reader.h"
#include "uefi_vars.h"
#include "esp_scanner.h"
#include "os_detector.h"
#include "boot_editor.h"
#include "backup.h"

// ────────────────────────────────────────────────────────────────────────────
// Console helpers
// ────────────────────────────────────────────────────────────────────────────

static void SetConsoleColor(WORD color)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

static void PrintHeader()
{
    SetConsoleColor(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    wprintf(L"\n");
    wprintf(L"  ╔══════════════════════════════════════════════════════════╗\n");
    wprintf(L"  ║          BCDToolWindows – EFI Boot Manager               ║\n");
    wprintf(L"  ║     Gestione avanzata delle voci di avvio UEFI           ║\n");
    wprintf(L"  ╚══════════════════════════════════════════════════════════╝\n");
    wprintf(L"\n");
    SetConsoleColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

static void PrintSeparator()
{
    wprintf(L"  %s\n", std::wstring(62, L'─').c_str());
}

static void PressEnterToContinue()
{
    wprintf(L"\n  Premi INVIO per continuare...");
    wchar_t dummy[4] = {};
    ReadWideLine(dummy, 4);
    wprintf(L"\n");
}

static int ReadInt(const wchar_t* prompt, int minVal, int maxVal)
{
    while (true)
    {
        wprintf(L"%s", prompt);
        wchar_t buf[32] = {};
        ReadWideLine(buf, 32);
        int val = _wtoi(buf);
        if (val >= minVal && val <= maxVal)
            return val;
        wprintf(L"  [!] Valore non valido. Inserisci un numero tra %d e %d.\n",
            minVal, maxVal);
    }
}

static std::wstring ReadLine(const wchar_t* prompt)
{
    wprintf(L"%s", prompt);
    wchar_t buf[256] = {};
    ReadWideLine(buf, 256);
    return buf;
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Visualizza voci di boot
// ────────────────────────────────────────────────────────────────────────────

static void MenuListEntries()
{
    PrintHeader();
    wprintf(L"  VOCI DI AVVIO UEFI CORRENTI\n");
    PrintSeparator();

    auto entries = ListAllBootEntries();
    PrintBootEntries(entries);

    auto order = GetBootOrder();
    wprintf(L"  BootOrder: ");
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (i > 0) wprintf(L" → ");
        wprintf(L"Boot%04X", order[i]);
    }
    wprintf(L"\n");

    PressEnterToContinue();
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Scansione dischi
// ────────────────────────────────────────────────────────────────────────────

static void MenuScanDisks()
{
    PrintHeader();
    wprintf(L"  SCANSIONE DISCHI E SISTEMI OPERATIVI RILEVATI\n");
    PrintSeparator();

    wprintf(L"\n  [*] Scansione dischi fisici...\n");
    auto disks = EnumerateDisks();

    for (const auto& disk : disks)
    {
        wprintf(L"\n  Disco %d: %s  [%.1f GB, %u B/settore]\n",
            disk.DiskIndex,
            disk.FriendlyName.c_str(),
            disk.DiskSizeBytes / 1073741824.0,
            disk.BytesPerSector);

        if (!disk.IsGPT)
        {
            wprintf(L"    (Non GPT – ignorato)\n");
            continue;
        }

        for (const auto& p : disk.Partitions)
        {
            wprintf(L"    Partizione %u: %s%s  [%.1f GB]  %s\n",
                p.PartitionNumber,
                p.PartitionName.c_str(),
                p.IsESP ? L" [ESP]" : L"",
                p.PartitionLengthBytes / 1073741824.0,
                p.UniquePartitionGUID.ToString().c_str());
        }
    }

    wprintf(L"\n  [*] Ricerca sistemi operativi nelle ESP...\n");
    auto esps    = FindAllESPs();
    auto mounted = FindAndMountESPs(esps);
    auto oses    = DetectAllOSes(mounted);

    if (oses.empty())
    {
        wprintf(L"  [!] Nessun sistema operativo trovato.\n");
    }
    else
    {
        wprintf(L"\n  Sistemi operativi trovati:\n");
        int idx = 1;
        for (const auto& os : oses)
        {
            wprintf(L"    %d. %-30s  %s\n",
                idx++,
                os.Name.c_str(),
                os.BootloaderPath.c_str());
        }
    }

    for (const auto& m : mounted)
        if (m.WasMountedByUs)
            ReleaseDriveLetter(m.DriveLetter);

    PressEnterToContinue();
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Aggiungi voce manualmente
// ────────────────────────────────────────────────────────────────────────────

static void MenuAddEntry()
{
    PrintHeader();
    wprintf(L"  AGGIUNGI VOCE DI AVVIO\n");
    PrintSeparator();

    wprintf(L"\n  [*] Ricerca sistemi operativi nelle ESP...\n");
    auto esps    = FindAllESPs();
    auto mounted = FindAndMountESPs(esps);
    auto oses    = DetectAllOSes(mounted);

    if (oses.empty())
    {
        wprintf(L"  [!] Nessun sistema operativo trovato automaticamente.\n");
        wprintf(L"  Usa 'Auto-configura tutto' per una rilevazione completa.\n");

        for (const auto& m : mounted)
            if (m.WasMountedByUs)
                ReleaseDriveLetter(m.DriveLetter);

        PressEnterToContinue();
        return;
    }

    wprintf(L"\n  Sistemi operativi disponibili (non ancora in lista):\n");

    // Filter out OSes that already have a boot entry
    auto existing = ListAllBootEntries();
    std::vector<DetectedOS> missing;

    for (const auto& os : oses)
    {
        bool found = false;
        for (const auto& ex : existing)
        {
            if (!ex.Entry.Valid) continue;
            bool samePartition = ex.Entry.HasHDPath &&
                ex.Entry.HD_PartitionGUID == os.ESPPartition.UniquePartitionGUID;
            auto fp = ex.Entry.FilePath;
            for (auto& c : fp) c = (wchar_t)towlower(c);
            auto bp = os.BootloaderPath;
            for (auto& c : bp) c = (wchar_t)towlower(c);
            if (samePartition && fp == bp) { found = true; break; }
        }
        if (!found)
            missing.push_back(os);
    }

    if (missing.empty())
    {
        wprintf(L"  [=] Tutti i sistemi operativi rilevati hanno già una voce.\n");

        for (const auto& m : mounted)
            if (m.WasMountedByUs)
                ReleaseDriveLetter(m.DriveLetter);

        PressEnterToContinue();
        return;
    }

    for (int i = 0; i < (int)missing.size(); ++i)
        wprintf(L"    %d. %-28s  [Disco %d, Partizione %u]  %s\n",
            i + 1,
            missing[i].Name.c_str(),
            missing[i].ESPPartition.DiskIndex,
            missing[i].ESPPartition.PartitionNumber,
            missing[i].BootloaderPath.c_str());

    wprintf(L"    0. Annulla\n");

    int choice = ReadInt(L"\n  Seleziona: ", 0, (int)missing.size());
    if (choice == 0)
    {
        for (const auto& m : mounted)
            if (m.WasMountedByUs)
                ReleaseDriveLetter(m.DriveLetter);
        return;
    }

    const DetectedOS& os = missing[choice - 1];

    // Allow customizing the name
    wprintf(L"\n  Nome voce [%s]: ", os.Name.c_str());
    wchar_t nameBuf[128] = {};
    ReadWideLine(nameBuf, 128);
    DetectedOS osToAdd = os;
    if (wcslen(nameBuf) > 0)
        osToAdd.Name = nameBuf;

    wprintf(L"  [*] Aggiunta in corso...\n");

    // Auto-save snapshot before first modification
    static bool snapshotSaved = false;
    if (!snapshotSaved)
    {
        std::wstring snapPath = DefaultSnapshotPath();
        if (SaveSnapshot(snapPath, L"Auto-backup prima delle modifiche"))
        {
            wprintf(L"  [+] Backup automatico salvato: %s\n", snapPath.c_str());
            snapshotSaved = true;
        }
    }

    uint16_t slot = AddBootEntry(osToAdd);
    if (slot != 0xFFFF)
        wprintf(L"  [+] Aggiunto Boot%04X: %s\n", slot, osToAdd.Name.c_str());
    else
        wprintf(L"  [!] Errore durante l'aggiunta della voce.\n");

    for (const auto& m : mounted)
        if (m.WasMountedByUs)
            ReleaseDriveLetter(m.DriveLetter);

    PressEnterToContinue();
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Rimuovi voce
// ────────────────────────────────────────────────────────────────────────────

static void MenuRemoveEntry()
{
    PrintHeader();
    wprintf(L"  RIMUOVI VOCE DI AVVIO\n");
    PrintSeparator();

    auto entries = ListAllBootEntries();
    if (entries.empty())
    {
        wprintf(L"  (nessuna voce)\n");
        PressEnterToContinue();
        return;
    }

    PrintBootEntries(entries);

    wprintf(L"  Inserisci il numero Boot (es. 0001) o 0 per annullare: ");
    wchar_t buf[16] = {};
    ReadWideLine(buf, 16);

    if (wcscmp(buf, L"0") == 0)
        return;

    wchar_t* endPtr = nullptr;
    uint16_t bootNum = (uint16_t)wcstoul(buf, &endPtr, 16);

    // Verify the entry exists
    bool found = false;
    for (const auto& e : entries)
        if (e.Entry.BootNum == bootNum) { found = true; break; }

    if (!found)
    {
        wprintf(L"  [!] Boot%04X non trovato.\n", bootNum);
        PressEnterToContinue();
        return;
    }

    wprintf(L"  Sei sicuro di voler rimuovere Boot%04X? [s/N]: ");
    wchar_t confirm[8] = {};
    ReadWideLine(confirm, 8);
    if (towlower(confirm[0]) != L's')
    {
        wprintf(L"  Annullato.\n");
        PressEnterToContinue();
        return;
    }

    // Auto-snapshot
    {
        std::wstring snapPath = DefaultSnapshotPath();
        if (SaveSnapshot(snapPath, L"Auto-backup prima di rimozione voce"))
            wprintf(L"  [+] Backup salvato: %s\n", snapPath.c_str());
    }

    if (RemoveBootEntry(bootNum))
        wprintf(L"  [+] Boot%04X rimosso con successo.\n", bootNum);
    else
        wprintf(L"  [!] Errore durante la rimozione.\n");

    PressEnterToContinue();
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Cambia ordine di avvio
// ────────────────────────────────────────────────────────────────────────────

static void MenuChangeOrder()
{
    while (true)
    {
        PrintHeader();
        wprintf(L"  CAMBIA ORDINE DI AVVIO\n");
        PrintSeparator();

        auto entries = ListAllBootEntries();
        PrintBootEntries(entries);

        wprintf(L"  Comandi:\n");
        wprintf(L"    su <Boot#hex>   – sposta una voce in su\n");
        wprintf(L"    giu <Boot#hex>  – sposta una voce in giu\n");
        wprintf(L"    q               – torna al menu principale\n");
        wprintf(L"\n  > ");

        wchar_t buf[32] = {};
        ReadWideLine(buf, 32);

        if (towlower(buf[0]) == L'q')
            break;

        wchar_t cmd[16] = {}, hexStr[16] = {};
        if (ScanWideTwo(buf, cmd, 16, hexStr, 16) < 2)
        {
            wprintf(L"  [!] Sintassi non valida.\n");
            Sleep(1000);
            continue;
        }

        wchar_t* endPtr = nullptr;
        uint16_t bootNum = (uint16_t)wcstoul(hexStr, &endPtr, 16);

        bool ok = false;
        if (_wcsicmp(cmd, L"su") == 0)
            ok = MoveEntryUp(bootNum);
        else if (_wcsicmp(cmd, L"giu") == 0)
            ok = MoveEntryDown(bootNum);
        else
        {
            wprintf(L"  [!] Comando non riconosciuto.\n");
            Sleep(1000);
            continue;
        }

        if (!ok)
        {
            wprintf(L"  [!] Impossibile spostare Boot%04X (già al limite?).\n", bootNum);
            Sleep(1000);
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Auto-configura tutto
// ────────────────────────────────────────────────────────────────────────────

static void MenuAutoConfigure()
{
    PrintHeader();
    wprintf(L"  AUTO-CONFIGURAZIONE\n");
    PrintSeparator();
    wprintf(L"\n  Questa funzione:\n");
    wprintf(L"   1. Salva un backup dello stato UEFI corrente\n");
    wprintf(L"   2. Scansiona tutti i dischi\n");
    wprintf(L"   3. Aggiunge voci per tutti i sistemi operativi trovati\n");
    wprintf(L"   4. Mette Windows al primo posto nel boot order\n");
    wprintf(L"\n  Continuare? [s/N]: ");

    wchar_t confirm[8] = {};
    ReadWideLine(confirm, 8);
    if (towlower(confirm[0]) != L's')
    {
        wprintf(L"  Annullato.\n");
        PressEnterToContinue();
        return;
    }

    // Save snapshot first
    std::wstring snapPath = DefaultSnapshotPath();
    if (SaveSnapshot(snapPath, L"Auto-backup prima dell'auto-configurazione"))
        wprintf(L"\n  [+] Backup salvato: %s\n", snapPath.c_str());
    else
        wprintf(L"\n  [!] Avviso: backup non riuscito. Continuando comunque...\n");

    int added = AutoConfigure();

    wprintf(L"\n  [+] Auto-configurazione completata. %d voce/i aggiunta/e.\n", added);
    PressEnterToContinue();
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Backup e ripristino
// ────────────────────────────────────────────────────────────────────────────

static void MenuBackupRestore()
{
    while (true)
    {
        PrintHeader();
        wprintf(L"  BACKUP E RIPRISTINO\n");
        PrintSeparator();
        wprintf(L"\n");
        wprintf(L"  1. Crea backup dello stato UEFI corrente\n");
        wprintf(L"  2. Ripristina da backup\n");
        wprintf(L"  3. Visualizza backup disponibili\n");
        wprintf(L"  0. Torna al menu principale\n");
        wprintf(L"\n  Scelta: ");

        wchar_t buf[8] = {};
        ReadWideLine(buf, 8);
        int choice = _wtoi(buf);

        if (choice == 0)
            break;

        if (choice == 1)
        {
            // Create backup
            std::wstring note = ReadLine(L"\n  Nota per il backup (opzionale): ");
            std::wstring snapPath = DefaultSnapshotPath();

            if (SaveSnapshot(snapPath, note.empty() ? L"Backup manuale" : note))
            {
                wprintf(L"  [+] Backup salvato:\n      %s\n", snapPath.c_str());

                // Count how many entries were saved
                UEFISnapshot snap;
                LoadSnapshot(snapPath, snap);
                wprintf(L"      Voci salvate: %zu, BootOrder: %zu elementi\n",
                    snap.BootEntries.size(), snap.BootOrder.size());
            }
            else
                wprintf(L"  [!] Errore nel salvataggio del backup.\n");

            PressEnterToContinue();
        }
        else if (choice == 2)
        {
            // Restore from backup
            auto snaps = ListSnapshots();
            if (snaps.empty())
            {
                wprintf(L"\n  [!] Nessun file di backup (.bcdtool) trovato nella cartella del programma.\n");
                PressEnterToContinue();
                continue;
            }

            wprintf(L"\n  Backup disponibili:\n");
            for (int i = 0; i < (int)snaps.size(); ++i)
            {
                UEFISnapshot snap;
                if (LoadSnapshot(snaps[i], snap))
                {
                    // Get just the filename
                    std::wstring fname = snaps[i];
                    size_t slash = fname.rfind(L'\\');
                    if (slash != std::wstring::npos)
                        fname = fname.substr(slash + 1);

                    wprintf(L"    %d. %s\n", i + 1, fname.c_str());
                    wprintf(L"       Creato: %s\n", snap.Timestamp.c_str());
                    wprintf(L"       Nota:   %s\n", snap.Note.c_str());
                    wprintf(L"       Voci:   %zu\n", snap.BootEntries.size());
                }
                else
                {
                    wprintf(L"    %d. %s (non leggibile)\n", i + 1,
                        snaps[i].c_str());
                }
            }
            wprintf(L"    0. Annulla\n");

            int snapChoice = ReadInt(L"\n  Seleziona backup: ", 0, (int)snaps.size());
            if (snapChoice == 0)
                continue;

            UEFISnapshot snap;
            if (!LoadSnapshot(snaps[snapChoice - 1], snap))
            {
                wprintf(L"  [!] Impossibile leggere il backup.\n");
                PressEnterToContinue();
                continue;
            }

            wprintf(L"\n  ATTENZIONE: Il ripristino sovrascriverà TUTTE le voci di avvio UEFI\n");
            wprintf(L"  con quelle del backup. Continuare? [s/N]: ");
            wchar_t confirm[8] = {};
            ReadWideLine(confirm, 8);
            if (towlower(confirm[0]) != L's')
            {
                wprintf(L"  Annullato.\n");
                PressEnterToContinue();
                continue;
            }

            // Save a backup of current state before restoring
            std::wstring prePath = DefaultSnapshotPath();
            if (SaveSnapshot(prePath, L"Auto-backup prima del ripristino"))
                wprintf(L"  [+] Backup dello stato attuale salvato: %s\n", prePath.c_str());

            wprintf(L"  [*] Ripristino in corso...\n");
            if (ApplySnapshot(snap))
                wprintf(L"  [+] Ripristino completato con successo.\n");
            else
                wprintf(L"  [!] Ripristino completato con alcuni errori (vedi sopra).\n");

            PressEnterToContinue();
        }
        else if (choice == 3)
        {
            // List snapshots
            auto snaps = ListSnapshots();
            if (snaps.empty())
            {
                wprintf(L"\n  Nessun file di backup trovato.\n");
            }
            else
            {
                wprintf(L"\n  File di backup trovati:\n");
                for (const auto& s : snaps)
                {
                    UEFISnapshot snap;
                    std::wstring fname = s;
                    size_t slash = fname.rfind(L'\\');
                    if (slash != std::wstring::npos)
                        fname = fname.substr(slash + 1);

                    if (LoadSnapshot(s, snap))
                        wprintf(L"    %s  [%s] %s\n",
                            fname.c_str(), snap.Timestamp.c_str(), snap.Note.c_str());
                    else
                        wprintf(L"    %s  [non valido]\n", fname.c_str());
                }
            }
            PressEnterToContinue();
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Menu: Impostazioni avanzate
// ────────────────────────────────────────────────────────────────────────────

static void MenuAdvanced()
{
    while (true)
    {
        PrintHeader();
        wprintf(L"  IMPOSTAZIONI AVANZATE\n");
        PrintSeparator();
        wprintf(L"\n");
        wprintf(L"  1. Imposta BootNext (avvio singolo diverso)\n");
        wprintf(L"  2. Abilita/Disabilita una voce\n");
        wprintf(L"  0. Torna al menu principale\n");
        wprintf(L"\n  Scelta: ");

        wchar_t buf[8] = {};
        ReadWideLine(buf, 8);
        int choice = _wtoi(buf);

        if (choice == 0)
            break;

        if (choice == 1)
        {
            auto entries = ListAllBootEntries();
            PrintBootEntries(entries);

            wprintf(L"  Numero Boot# per il prossimo avvio (hex, es. 0002) o 0 per annullare: ");
            wchar_t hexBuf[16] = {};
            ReadWideLine(hexBuf, 16);
            if (wcscmp(hexBuf, L"0") == 0)
                continue;

            wchar_t* endPtr = nullptr;
            uint16_t bootNum = (uint16_t)wcstoul(hexBuf, &endPtr, 16);

            if (SetBootNext(bootNum))
                wprintf(L"  [+] BootNext impostato su Boot%04X. Effettivo al prossimo riavvio.\n", bootNum);
            else
                wprintf(L"  [!] Errore nell'impostazione di BootNext.\n");

            PressEnterToContinue();
        }
        else if (choice == 2)
        {
            auto entries = ListAllBootEntries();
            PrintBootEntries(entries);

            wprintf(L"  Numero Boot# da abilitare/disabilitare (hex): ");
            wchar_t hexBuf[16] = {};
            ReadWideLine(hexBuf, 16);

            wchar_t* endPtr = nullptr;
            uint16_t bootNum = (uint16_t)wcstoul(hexBuf, &endPtr, 16);

            // Find current state
            bool currentlyActive = false;
            for (const auto& e : entries)
                if (e.Entry.BootNum == bootNum)
                    currentlyActive = e.IsActive;

            bool newState = !currentlyActive;
            if (SetEntryActive(bootNum, newState))
                wprintf(L"  [+] Boot%04X ora è %s.\n",
                    bootNum, newState ? L"ABILITATO" : L"DISABILITATO");
            else
                wprintf(L"  [!] Errore.\n");

            PressEnterToContinue();
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Controllo UEFI supporto
// ────────────────────────────────────────────────────────────────────────────

static bool CheckUEFISupport()
{
    // Try to read BootOrder to see if UEFI variables are accessible
    DWORD r = GetFirmwareEnvironmentVariableW(
        L"BootOrder",
        L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}",
        nullptr, 0);
    DWORD err = GetLastError();

    // ERROR_INSUFFICIENT_BUFFER means the variable exists (UEFI is accessible)
    // ERROR_INVALID_FUNCTION means BIOS (not UEFI)
    // ERROR_ACCESS_DENIED means we need admin rights
    if (err == ERROR_INVALID_FUNCTION)
    {
        wprintf(L"\n  [!] ERRORE: Il sistema è avviato in modalità Legacy BIOS, non UEFI.\n");
        wprintf(L"      Questo programma richiede un sistema con firmware UEFI.\n\n");
        return false;
    }
    if (err == ERROR_ACCESS_DENIED)
    {
        wprintf(L"\n  [!] ERRORE: Accesso negato alle variabili UEFI.\n");
        wprintf(L"      Esegui il programma come Amministratore.\n\n");
        return false;
    }
    return true;  // err == ERROR_INSUFFICIENT_BUFFER or r > 0
}

// ────────────────────────────────────────────────────────────────────────────
// Menu principale
// ────────────────────────────────────────────────────────────────────────────

static void MainMenu()
{
    while (true)
    {
        PrintHeader();
        wprintf(L"  MENU PRINCIPALE\n");
        PrintSeparator();
        wprintf(L"\n");
        wprintf(L"  1. Visualizza voci di avvio correnti\n");
        wprintf(L"  2. Scansiona dischi e rileva sistemi operativi\n");
        wprintf(L"  3. Aggiungi voce di avvio\n");
        wprintf(L"  4. Rimuovi voce di avvio\n");
        wprintf(L"  5. Cambia ordine di avvio\n");
        wprintf(L"  6. Auto-configura tutto (consigliato)\n");
        wprintf(L"  7. Backup e ripristino\n");
        wprintf(L"  8. Impostazioni avanzate\n");
        wprintf(L"  0. Esci\n");
        wprintf(L"\n  Scelta: ");

        wchar_t buf[8] = {};
        ReadWideLine(buf, 8);
        int choice = _wtoi(buf);

        switch (choice)
        {
            case 0: return;
            case 1: MenuListEntries();    break;
            case 2: MenuScanDisks();      break;
            case 3: MenuAddEntry();       break;
            case 4: MenuRemoveEntry();    break;
            case 5: MenuChangeOrder();    break;
            case 6: MenuAutoConfigure();  break;
            case 7: MenuBackupRestore();  break;
            case 8: MenuAdvanced();       break;
            default:
                wprintf(L"  [!] Scelta non valida.\n");
                Sleep(500);
                break;
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Entry point
// ────────────────────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[])
{
    // Set UTF-8 / wide output
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    setvbuf(stdout, nullptr, _IONBF, 0);  // unbuffered — prompts appear immediately

    // Elevate to Administrator if not already
    EnsureAdminOrElevate(argc, argv);

    // Set console title
    SetConsoleTitleW(L"BCDToolWindows – EFI Boot Manager");

    // Acquire required privileges
    if (!AcquireAllRequiredPrivileges())
    {
        wprintf(L"\n  [!] Impossibile acquisire i privilegi necessari.\n");
        wprintf(L"      Assicurati di eseguire come Amministratore.\n");
        system("pause");
        return 1;
    }

    // Verify UEFI firmware
    if (!CheckUEFISupport())
    {
        system("pause");
        return 1;
    }

    MainMenu();

    return 0;
}
