# BCDToolWindows — EFI Boot Manager per Windows

> Gestione diretta delle variabili UEFI della scheda madre da Windows, senza GRUB, senza dipendenze tra dischi.

---

## Indice

0. [Guida rapida: Linux Mint su secondo disco](#0-guida-rapida-linux-mint-su-secondo-disco)
1. [Perché questo progetto](#1-perché-questo-progetto)
2. [Come funziona il firmware UEFI](#2-come-funziona-il-firmware-uefi)
3. [Architettura del software](#3-architettura-del-software)
4. [File per file: spiegazione dettagliata](#4-file-per-file-spiegazione-dettagliata)
   - [efi_types.h — Strutture dati EFI e GPT](#41-efi_typesh--strutture-dati-efi-e-gpt)
   - [privilege.h / privilege.cpp — Gestione privilegi](#42-privilegeh--privilegecpp--gestione-privilegi)
   - [gpt_reader.h / gpt_reader.cpp — Lettura dischi GPT](#43-gpt_readerh--gpt_readercpp--lettura-dischi-gpt)
   - [uefi_vars.h / uefi_vars.cpp — Variabili UEFI](#44-uefi_varsh--uefi_varscpp--variabili-uefi)
   - [esp_scanner.h / esp_scanner.cpp — Scanner partizioni ESP](#45-esp_scannerh--esp_scannercpp--scanner-partizioni-esp)
   - [os_detector.h / os_detector.cpp — Rilevamento OS](#46-os_detectorh--os_detectorcpp--rilevamento-os)
   - [boot_editor.h / boot_editor.cpp — Editor voci di boot](#47-boot_editorh--boot_editorcpp--editor-voci-di-boot)
   - [backup.h / backup.cpp — Backup e ripristino](#48-backuph--backupcpp--backup-e-ripristino)
   - [main.cpp — Interfaccia utente](#49-maincpp--interfaccia-utente)
5. [Catene di chiamata per ogni voce di menu](#5-catene-di-chiamata-per-ogni-voce-di-menu)
6. [Formato binario delle variabili UEFI](#6-formato-binario-delle-variabili-uefi)
7. [Formato binario del file di backup .bcdtool](#7-formato-binario-del-file-di-backup-bcdtool)
8. [Compatibilità OS e sicurezza](#8-compatibilità-os-e-sicurezza)
9. [Requisiti, compilazione e utilizzo](#9-requisiti-compilazione-e-utilizzo)

---

## 1. Perché questo progetto

### Il problema del dual boot classico

Quando installi Linux su un secondo disco fisico e usi GRUB come bootloader, succede questo:

```
[UEFI Firmware]
      │
      └── avvia → GRUB (sul disco Linux, nella sua partizione ESP)
                       │
                       ├── Ubuntu (stesso disco)
                       └── Windows (disco diverso)
```

**Problema critico:** se stacchi o rimuovi il disco Linux, il firmware prova ad avviare GRUB che non esiste più → errore `Boot device not found` o schermata nera.

### La soluzione di BCDToolWindows

BCDToolWindows scrive le voci di avvio **direttamente nella NVRAM della scheda madre**, senza intermediari:

```
[UEFI Firmware — NVRAM della scheda madre]
      │
      ├── Boot0001: Windows Boot Manager → Disco 0, Partizione 2, \EFI\Microsoft\Boot\bootmgfw.efi
      ├── Boot0002: Ubuntu              → Disco 1, Partizione 1, \EFI\ubuntu\grubx64.efi
      └── BootOrder: 0001, 0002
```

Ogni voce è **completamente indipendente**. Se rimuovi il disco Linux:
- Il firmware cerca `Boot0002` → trova che la partizione con quel GUID non esiste
- Salta automaticamente alla voce successiva → `Boot0001` → Windows si avvia
- Nessun errore, nessun intervento manuale

---

## 2. Come funziona il firmware UEFI

### NVRAM: la memoria non volatile della scheda madre

Il firmware UEFI mantiene un database di variabili in una zona di memoria flash chiamata **NVRAM** (Non-Volatile RAM). Queste variabili sopravvivono ai riavvii e sono totalmente indipendenti dai dischi connessi.

Le variabili di boot appartengono al **namespace EFI Global Variable**, identificato dal GUID:
```
{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}
```

### Le variabili rilevanti

| Nome variabile | Tipo | Descrizione |
|---|---|---|
| `BootOrder` | `uint16_t[]` | Lista ordinata degli slot di boot (es. `[0x0001, 0x0002, 0x0000]`) |
| `Boot0000` … `BootFFFF` | `EFI_LOAD_OPTION` | Una singola voce di avvio con path del bootloader |
| `BootCurrent` | `uint16_t` | Slot usato per l'avvio corrente (sola lettura) |
| `BootNext` | `uint16_t` | Override per il prossimo avvio (si cancella da solo dopo) |

### Sequenza di avvio del firmware UEFI

```
Accensione
    │
    ├── POST (Power-On Self-Test)
    │
    ├── Legge BootOrder dalla NVRAM: [0x0001, 0x0002]
    │
    ├── Tenta Boot0001:
    │       ├── Cerca disco con partizione GUID = {aaaa-...}  ✓ Trovata
    │       ├── Legge FAT32 dalla partizione
    │       ├── Apre \EFI\Microsoft\Boot\bootmgfw.efi
    │       └── Trasferisce controllo → Windows si avvia
    │
    └── (se Boot0001 fallisce, tenta Boot0002, poi Boot0003, ...)
```

### Accesso Windows alle variabili UEFI

Windows espone le variabili UEFI tramite due funzioni del kernel:

```c
// Lettura (in hal.dll → NtQuerySystemEnvironmentValueEx kernel)
DWORD GetFirmwareEnvironmentVariableW(
    LPCWSTR lpName,      // Nome: L"BootOrder"
    LPCWSTR lpGuid,      // Namespace: L"{8BE4DF61-...}"
    PVOID   pBuffer,     // Buffer di output
    DWORD   nSize        // Dimensione buffer
);

// Scrittura (in hal.dll → NtSetSystemEnvironmentValueEx kernel)
BOOL SetFirmwareEnvironmentVariableW(
    LPCWSTR lpName,      // Nome: L"Boot0001"
    LPCWSTR lpGuid,      // Namespace: L"{8BE4DF61-...}"
    PVOID   pValue,      // Dati da scrivere
    DWORD   nSize        // 0 = elimina la variabile
);
```

Queste chiamate richiedono il privilegio `SE_SYSTEM_ENVIRONMENT_PRIVILEGE` nel token del processo.

---

## 3. Architettura del software

```
main.cpp  (UI console — menu interattivo)
    │
    ├── boot_editor.cpp  (logica ad alto livello)
    │       ├── uefi_vars.cpp    (lettura/scrittura variabili UEFI)
    │       ├── gpt_reader.cpp   (lettura tabella GPT dai dischi fisici)
    │       ├── esp_scanner.cpp  (montaggio partizioni ESP)
    │       └── os_detector.cpp  (database bootloader noti)
    │
    └── backup.cpp       (backup/ripristino stato UEFI)
            └── uefi_vars.cpp    (lettura/scrittura variabili UEFI)

Tutti i file usano:
    ├── efi_types.h      (strutture dati condivise)
    └── privilege.cpp    (gestione privilegi Windows)
```

### Dipendenze tra i moduli

```
efi_types.h ← (tutti)
privilege   ← main
gpt_reader  ← esp_scanner, boot_editor, main
uefi_vars   ← boot_editor, backup, main
esp_scanner ← boot_editor, main
os_detector ← boot_editor, main
boot_editor ← main
backup      ← main, boot_editor
```

---

## 4. File per file: spiegazione dettagliata

---

### 4.1 `efi_types.h` — Strutture dati EFI e GPT

Questo file è incluso da **tutti gli altri**. Definisce i tipi di dati fondamentali usando `#pragma pack(push, 1)` per garantire che le strutture abbiano esattamente la dimensione specificata dallo standard UEFI — senza padding aggiunto dal compilatore.

#### Direttiva `#pragma pack(push, 1)`

Il compilatore C++ normalmente aggiunge byte di "padding" tra i campi di una struct per allinearli in memoria. Per le strutture EFI, questo è **vietato** — il firmware si aspetta i byte esattamente nelle posizioni dello standard. `#pragma pack(push, 1)` forza l'allineamento a 1 byte.

#### Struttura `EFI_GUID` (16 byte)

Un GUID (Globally Unique Identifier) è un numero a 128 bit usato per identificare univocamente partizioni, namespace di variabili, dispositivi, ecc.

```
Layout in memoria (16 byte totali):
Offset  Size  Campo    Esempio per ESP GUID
──────  ────  ───────  ──────────────────────────────────────
  0     4     Data1    0x28 0x73 0x2A 0xC1  (= 0xC12A7328 in little-endian)
  4     2     Data2    0x81 0xF1             (= 0xF181 → 0xF81F in little-endian)
  6     2     Data3    0xD2 0x11             (= 0x11D2)
  8     8     Data4    0xBA 0x4B 0x00 0xA0 0xC9 0x3E 0xC9 0x3B
```

Il GUID `{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}` è lo standard UEFI per identificare una **EFI System Partition**. È fisso per tutti i dischi e tutti i sistemi.

Il metodo `ToString()` formatta il GUID come stringa leggibile:
```cpp
swprintf_s(buf, 64,
    L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
    Data1, Data2, Data3,
    Data4[0], Data4[1], Data4[2], ...);
```

#### Struttura `EFI_DEVICE_PATH_PROTOCOL` (4 byte — header generico)

Ogni nodo di un percorso dispositivo EFI inizia con questo header:

```
Offset  Size  Campo    Significato
──────  ────  ───────  ──────────────────────────────────────────
  0     1     Type     Categoria del nodo (0x04 = Media)
  1     1     SubType  Tipo specifico (0x01 = HardDrive, 0x04 = FilePath)
  2     2     Length   Lunghezza TOTALE di questo nodo (incluso l'header)
```

#### Struttura `HARDDRIVE_DEVICE_PATH` (42 byte esatti)

Identifica una specifica partizione su disco. È il nodo più importante per il boot da GPT.

```
Offset  Size  Campo            Significato
──────  ────  ───────────────  ──────────────────────────────────────────────────
  0     1     Header.Type      = 0x04  (MEDIA_DEVICE_PATH)
  1     1     Header.SubType   = 0x01  (MEDIA_HARDDRIVE_DP)
  2     2     Header.Length    = 42    (0x002A, in little-endian: 0x2A 0x00)
  4     4     PartitionNumber  Numero della partizione (1-based, es. 0x01 0x00 0x00 0x00)
  8     8     PartitionStart   LBA iniziale (es. 0x0800 = settore 2048)
 16     8     PartitionSize    Numero di LBA della partizione (es. 0x82000)
 24    16     Signature        Per GPT: UniquePartitionGUID (il GUID UNICO di QUESTA partizione)
 40     1     MBRType          = 0x02  (GPT; 0x01 = MBR legacy)
 41     1     SignatureType    = 0x02  (Signature contiene un GUID; 0x01 = contiene firma MBR)
```

Il campo `Signature[16]` è il cuore del sistema: contiene il **GUID univoco della partizione ESP**, letto dalla tabella GPT del disco. Se quel disco viene rimosso, non esiste più nessuna partizione con quel GUID, e il firmware salta la voce.

La `static_assert` verifica a compile-time che la struct abbia davvero 42 byte:
```cpp
static_assert(sizeof(HARDDRIVE_DEVICE_PATH) == 42, "Wrong HARDDRIVE_DEVICE_PATH size");
```

#### Struttura `GPT_HEADER` (92 byte)

L'intestazione della tabella delle partizioni GPT, sempre al **LBA 1** (512 byte dall'inizio del disco).

```
Offset  Size  Campo                      Significato
──────  ────  ─────────────────────────  ─────────────────────────────────────────
  0     8     Signature                  = 0x5452415020494645 ("EFI PART" in ASCII)
  8     4     Revision                   = 0x00010000 (versione 1.0)
 12     4     HeaderSize                 Normalmente 92 byte
 16     4     HeaderCRC32                CRC32 dell'header (calcolato con questo campo = 0)
 20     4     Reserved                   = 0
 24     8     MyLBA                      LBA di questo header (sempre 1)
 32     8     AlternateLBA               LBA dell'header di backup (ultimo settore del disco)
 40     8     FirstUsableLBA             Primo LBA usabile per le partizioni
 48     8     LastUsableLBA              Ultimo LBA usabile
 56    16     DiskGUID                   GUID univoco del disco intero
 72     8     PartitionEntryLBA          LBA della tabella delle partizioni (di solito 2)
 80     4     NumberOfPartitionEntries   Numero massimo di partizioni (di solito 128)
 84     4     SizeOfPartitionEntry       Dimensione di ogni entry (128 byte)
 88     4     PartitionEntryArrayCRC32   CRC32 dell'array di partizioni
```

#### Struttura `GPT_PARTITION_ENTRY` (128 byte per entry standard)

```
Offset  Size  Campo                Significato
──────  ────  ───────────────────  ────────────────────────────────────────────────
  0    16     PartitionTypeGUID    Tipo di partizione (es. ESP GUID, Microsoft GUID)
 16    16     UniquePartitionGUID  GUID UNICO di questa specifica partizione
 32     8     StartingLBA          Primo settore
 40     8     EndingLBA            Ultimo settore (incluso)
 48     8     Attributes           Bit flags (bit 2 = Required, bit 60 = Read-only, ecc.)
 56    72     PartitionName        Nome UTF-16LE (36 caratteri max, null-padded)
```

Il `UniquePartitionGUID` è quello che viene copiato nel campo `Signature` del `HARDDRIVE_DEVICE_PATH`.

#### Costanti `LOAD_OPTION_*`

```cpp
#define LOAD_OPTION_ACTIVE              0x00000001  // La voce è abilitata
#define LOAD_OPTION_FORCE_RECONNECT     0x00000002  // Riconnetti tutti i driver prima del boot
#define LOAD_OPTION_HIDDEN              0x00000008  // Non mostrare nel menu del firmware
```

---

### 4.2 `privilege.h / privilege.cpp` — Gestione privilegi

#### Perché servono i privilegi

Windows implementa il **principio del minimo privilegio**: anche un Amministratore non ha automaticamente accesso a tutto. Per operazioni sensibili come scrivere variabili UEFI o leggere dischi raw, il processo deve **esplicitamente richiedere** i privilegi necessari.

#### Funzione `IsRunningAsAdmin()`

```
Ritorna: bool — true se il processo ha diritti di Amministratore
```

Verifica se il token di sicurezza del processo corrente include il gruppo `BUILTIN\Administrators`:

1. `SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY`  
   Inizializza l'autorità NT (il "dominio" di sicurezza Windows)

2. `AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, ...)`  
   Crea un SID (Security Identifier) che rappresenta il gruppo Administrators locale

3. `CheckTokenMembership(nullptr, adminGroup, &isAdmin)`  
   Verifica se il token del processo corrente (nullptr = processo corrente) contiene quel SID

4. `FreeSid(adminGroup)` — libera la memoria allocata per il SID

#### Funzione `AcquirePrivilege(const wchar_t* privilegeName)`

```
Parametri: privilegeName — nome del privilegio, es. L"SeSystemEnvironmentPrivilege"
Ritorna: bool — true se il privilegio è stato acquisito
```

Abilita un privilegio nel token del processo:

1. `OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)`  
   Apre il token di sicurezza del processo corrente. Richiede i flag `TOKEN_ADJUST_PRIVILEGES` (per modificare i privilegi) e `TOKEN_QUERY` (per leggere i LUID).  
   `hToken` è un `HANDLE` — un numero intero opaco che rappresenta la risorsa nel kernel.

2. `TOKEN_PRIVILEGES tp = {}`  
   Struttura che descrive i privilegi da modificare:
   ```
   tp.PrivilegeCount = 1
   tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED
   tp.Privileges[0].Luid = ?  (compilato da LookupPrivilegeValueW)
   ```

3. `LookupPrivilegeValueW(nullptr, privilegeName, &tp.Privileges[0].Luid)`  
   Traduce il nome stringa del privilegio nel suo LUID (Locally Unique Identifier) — un numero a 64 bit che identifica il privilegio nel sistema corrente. Il LUID può cambiare tra avvii diversi.

4. `AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)`  
   Modifica i privilegi del token. Il secondo parametro `FALSE` significa "non disabilitare tutti i privilegi", ma solo modificare quelli indicati.

5. Controllo `GetLastError() == ERROR_SUCCESS`: la funzione `AdjustTokenPrivileges` può ritornare TRUE anche se non ha potuto abilitare tutti i privilegi richiesti — bisogna sempre verificare `GetLastError()` separatamente.

#### Funzione `EnsureAdminOrElevate(int argc, wchar_t* argv[])`

Se il processo non ha diritti Admin, rilancia se stesso tramite UAC:

1. `GetModuleFileNameW(nullptr, exePath, MAX_PATH)` — ottiene il percorso dell'exe corrente
2. Ricostruisce la command line dai parametri `argv[]`
3. `SHELLEXECUTEINFOW sei` — struttura per ShellExecute:
   - `sei.lpVerb = L"runas"` — indica a Windows di richiedere elevazione UAC
   - `sei.lpFile = exePath` — il file da eseguire
   - `sei.nShow = SW_NORMAL` — mostra la finestra normalmente
4. `ShellExecuteExW(&sei)` — lancia il processo elevato. Windows mostra il dialogo UAC.
5. `ExitProcess(0)` — termina il processo non elevato (quello corrente)

#### Funzione `AcquireAllRequiredPrivileges()`

Acquisisce tre privilegi:
- `SE_SYSTEM_ENVIRONMENT_NAME` (`L"SeSystemEnvironmentPrivilege"`) → accesso alle variabili UEFI
- `SE_MANAGE_VOLUME_NAME` (`L"SeManageVolumePrivilege"`) → I/O raw sui dischi
- `SE_BACKUP_NAME` (`L"SeBackupPrivilege"`) → accesso ai filesystem come backup operator

---

### 4.3 `gpt_reader.h / gpt_reader.cpp` — Lettura dischi GPT

Questo modulo interroga Windows sul layout fisico dei dischi, senza leggere il disco settore per settore (usa invece le IOCTL del kernel).

#### Struttura `DiskInfo`

```cpp
struct DiskInfo {
    int                        DiskIndex;       // 0-based: 0 = PhysicalDrive0
    UINT64                     DiskSizeBytes;   // Dimensione totale in byte
    UINT32                     BytesPerSector;  // 512 o 4096 (disco 4Kn)
    std::wstring               FriendlyName;    // Es. L"PhysicalDrive0"
    std::vector<PartitionInfo> Partitions;      // Tutte le partizioni GPT
    bool                       IsGPT;           // false se MBR o non inizializzato
};
```

#### Struttura `PartitionInfo`

```cpp
struct PartitionInfo {
    int          DiskIndex;              // Su quale disco fisico
    UINT32       PartitionNumber;        // 1-based (come in Windows Disk Management)
    EFI_GUID     PartitionTypeGUID;      // Tipo: ESP, BasicData, LinuxData, ecc.
    EFI_GUID     UniquePartitionGUID;    // GUID univoco di QUESTA partizione specifica
    UINT64       StartingOffsetBytes;    // Offset dall'inizio disco in BYTE
    UINT64       PartitionLengthBytes;   // Dimensione in BYTE
    UINT64       StartLBA;              // StartingOffsetBytes / BytesPerSector
    UINT64       SizeLBA;               // PartitionLengthBytes / BytesPerSector
    UINT32       BytesPerSector;        // Copia dal disco padre
    std::wstring PartitionName;         // Nome GPT (es. L"EFI System Partition")
    bool         IsESP;                 // true se PartitionTypeGUID == ESP_GUID
};
```

#### Funzione statica `OpenDisk(int index, bool readOnly)`

```
Parametri:
  index    — numero del disco fisico (0, 1, 2, ...)
  readOnly — true per aprire in sola lettura
Ritorna: HANDLE — handle al disco, o INVALID_HANDLE_VALUE se non esiste
```

Apre il disco come un file speciale del kernel Windows:

```cpp
wchar_t path[32];
swprintf_s(path, 32, L"\\\\.\\PhysicalDrive%d", index);
// Risultato: L"\\.\PhysicalDrive0", L"\\.\PhysicalDrive1", ecc.

return CreateFileW(path, GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,  // Permetti ad altri processi di leggere/scrivere
    nullptr,                              // Nessun security descriptor
    OPEN_EXISTING,                        // Il dispositivo deve già esistere
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING,  // No cache (I/O diretto)
    nullptr);
```

`FILE_FLAG_NO_BUFFERING` è necessario per leggere device fisici: garantisce che le letture siano allineate ai settori.

#### Funzione statica `GetBytesPerSector(HANDLE hDisk)`

Usa `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` per interrogare il firmware del disco:

```cpp
DISK_GEOMETRY_EX geo = {};
DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
    nullptr, 0,          // Input: nessuno
    &geo, sizeof(geo),   // Output: struttura DISK_GEOMETRY_EX
    &bytesReturned, nullptr);
return geo.Geometry.BytesPerSector;
```

`DISK_GEOMETRY_EX` contiene un campo `Geometry.BytesPerSector`: tipicamente 512, ma 4096 per dischi "4Kn" (Advanced Format moderni).

#### Funzione statica `ReadDiskLayout(int diskIndex, DiskInfo& out)`

Il cuore della lettura GPT. Usa `IOCTL_DISK_GET_DRIVE_LAYOUT_EX`:

```cpp
// Buffer grande abbastanza per 128 partizioni
const DWORD bufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                      128 * sizeof(PARTITION_INFORMATION_EX);
std::vector<BYTE> layoutBuf(bufSize, 0);
auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf.data());

DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
    nullptr, 0, layout, bufSize, &bytesReturned, nullptr);
```

La struttura `DRIVE_LAYOUT_INFORMATION_EX` restituita:

```
Offset  Campo                    Significato
──────  ───────────────────────  ─────────────────────────────────
  0     PartitionStyle           1 = GPT, 0 = MBR, 2 = RAW
  4     PartitionCount           Numero di partizioni trovate
  8     Gpt.DiskId               GUID del disco (solo per GPT)
 ...    PartitionEntry[0..N-1]   Array di PARTITION_INFORMATION_EX
```

Per ogni `PARTITION_INFORMATION_EX`:
```
Campo                  Significato
─────────────────────  ───────────────────────────────────────────────────
PartitionStyle         Deve essere PARTITION_STYLE_GPT (1)
PartitionNumber        0 = non assegnata, ≥1 = partizione reale
StartingOffset         Offset in byte dall'inizio disco (LARGE_INTEGER)
PartitionLength        Dimensione in byte (LARGE_INTEGER)
Gpt.PartitionType      GUID tipo (es. ESP, BasicData)
Gpt.PartitionId        GUID univoco questa partizione
Gpt.Attributes         Bit flags (Required, Hidden, ecc.)
Gpt.Name               Nome UTF-16LE (36 caratteri)
```

Il codice poi:
1. Converte offset e dimensione da byte a LBA: `StartingOffset / BytesPerSector`
2. Copia i GUID usando `memcmp` — funziona perché `EFI_GUID` e Windows `GUID` hanno lo stesso layout binario
3. Marca `IsESP = true` se `PartitionTypeGUID == ESP_PARTITION_TYPE_GUID`

#### Funzione `EnumerateDisks()`

```
Ritorna: std::vector<DiskInfo> — tutti i dischi fisici trovati
```

Scansiona da `PhysicalDrive0` a `PhysicalDrive31`. Si ferma al primo disco che non si riesce ad aprire (non necessariamente dopo aver trovato tutti i dischi, ma nella pratica funziona perché i numeri sono sequenziali).

#### Funzione `FindAllESPs()`

```
Ritorna: std::vector<PartitionInfo> — tutte le partizioni ESP trovate
```

Chiama `EnumerateDisks()` e filtra le partizioni con `IsESP == true`.

---

### 4.4 `uefi_vars.h / uefi_vars.cpp` — Variabili UEFI

Questo è il modulo più critico: legge e scrive i dati reali nella NVRAM della scheda madre.

#### Costante `EFI_GLOBAL_GUID_STR`

```cpp
static const wchar_t* EFI_GLOBAL_GUID_STR = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
```

Questo è il GUID del namespace "EFI Global Variable" — tutte le variabili standard UEFI (BootOrder, Boot0001, ecc.) appartengono a questo namespace. La funzione `GetFirmwareEnvironmentVariableW` richiede il GUID come stringa in questo formato preciso.

#### Struttura `EFIBootEntry` (struttura interna C++)

Una rappresentazione **parsata** e accessibile di un `EFI_LOAD_OPTION`:

```cpp
struct EFIBootEntry {
    uint16_t     BootNum;           // Es. 0x0001 per "Boot0001"
    uint32_t     Attributes;        // LOAD_OPTION_ACTIVE | ...
    std::wstring Description;       // L"Ubuntu", L"Windows Boot Manager", ecc.
    bool         Valid;             // false se il parsing è fallito

    // Dati estratti dal primo nodo HARDDRIVE_DEVICE_PATH
    bool         HasHDPath;
    uint32_t     HD_PartitionNumber;
    uint64_t     HD_PartitionStart; // LBA (dalla variabile UEFI)
    uint64_t     HD_PartitionSize;  // LBA
    EFI_GUID     HD_PartitionGUID;  // UniquePartitionGUID della partizione
    uint8_t      HD_MBRType;        // 0x02 = GPT
    uint8_t      HD_SignatureType;  // 0x02 = GUID

    std::wstring FilePath;          // Es. L"\\EFI\\ubuntu\\grubx64.efi"

    // Blob raw del device path (per riscrittura fedele)
    std::vector<uint8_t> RawDevicePath;
};
```

#### Funzione statica `ReadUEFIVar(const wchar_t* varName)`

```
Parametri: varName — nome della variabile (es. L"BootOrder", L"Boot0001")
Ritorna: std::vector<uint8_t> — contenuto grezzo, o vuoto se non esiste/errore
```

**Primo tentativo (dimensione):**
```cpp
DWORD needed = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR, nullptr, 0);
DWORD err = GetLastError();
```

Il comportamento varia per firmware:
- Se `needed > 0`: alcuni firmware restituiscono la dimensione direttamente
- Se `needed == 0` e `err == ERROR_INSUFFICIENT_BUFFER`: la variabile esiste, usiamo 4096 come stima
- Se `needed == 0` e altro errore: la variabile non esiste → ritorna `{}`

**Secondo tentativo (lettura):**
```cpp
std::vector<uint8_t> buf(needed, 0);  // Alloca 'needed' byte inizializzati a 0
DWORD got = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
    buf.data(), (DWORD)buf.size());
```

`buf.data()` ritorna un puntatore `uint8_t*` al buffer allocato sull'heap.

Se `got > 0`: `buf.resize(got)` taglia il buffer alla dimensione effettiva.

#### Funzione statica `WriteUEFIVar(const wchar_t* varName, const void* data, DWORD size)`

```cpp
return SetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
    const_cast<void*>(data), size) != FALSE;
```

Il `const_cast<void*>` rimuove il `const` richiesto dalla firma della Windows API (che non modifica il buffer, ma non è dichiarata const-correct).

#### Funzione statica `DeleteUEFIVar(const wchar_t* varName)`

```cpp
// Scrivere size=0 (o nullptr con size=0) elimina la variabile dalla NVRAM
return SetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR, nullptr, 0) != FALSE;
```

#### Funzione `GetBootOrder()`

```
Ritorna: std::vector<uint16_t> — lista di numeri di slot in ordine di priorità
```

1. `ReadUEFIVar(L"BootOrder")` → raw bytes (es. `[01 00 02 00 00 00]`)
2. Loop che interpreta coppie di byte come `uint16_t` in little-endian:
   ```cpp
   uint16_t num = (uint16_t)(raw[i] | (raw[i + 1] << 8));
   ```
   - `raw[0]=0x01, raw[1]=0x00` → `0x0001` → Boot0001
   - `raw[2]=0x02, raw[3]=0x00` → `0x0002` → Boot0002

**Esempio concreto:**
```
NVRAM BootOrder: 01 00 02 00 00 00
                 ↑──↑  ↑──↑  ↑──↑
                 0x0001 0x0002 0x0000
Risultato: [1, 2, 0]  → Boot0001 primo, Boot0002 secondo, Boot0000 terzo
```

#### Funzione `SetBootOrder(const std::vector<uint16_t>& order)`

Operazione inversa: converte i `uint16_t` in coppie di byte little-endian e scrive:

```cpp
std::vector<uint8_t> raw(order.size() * 2);
for (size_t i = 0; i < order.size(); ++i) {
    raw[i * 2]     = (uint8_t)(order[i] & 0xFF);     // byte basso
    raw[i * 2 + 1] = (uint8_t)(order[i] >> 8);        // byte alto
}
WriteUEFIVar(L"BootOrder", raw.data(), (DWORD)raw.size());
```

Se `order` è vuoto → elimina la variabile `BootOrder`.

#### Funzione statica `ParseLoadOption(uint16_t bootNum, const std::vector<uint8_t>& raw)`

Analizza il blob binario di una variabile `BootXXXX` e popola una struttura `EFIBootEntry`.

**Layout binario analizzato:**

```
Posizione  Dimensione  Campo
─────────  ──────────  ─────────────────────────────────────────────────
    p+0         4      Attributes (uint32, little-endian)
    p+4         2      FilePathListLength (uint16, little-endian)
    p+6      variabile Description (UTF-16LE, terminata da 0x0000)
    p+6+N    fpLen     Device path list (N = (descLen+1)*2)
    resto    variabile OptionalData (ignorato)
```

**Parsing Attributes:**
```cpp
e.Attributes = *(const uint32_t*)p;  // Reinterpreta 4 byte come uint32
p += 4;
```

**Parsing Description (stringa UTF-16):**
```cpp
const wchar_t* descStart = reinterpret_cast<const wchar_t*>(p);
size_t descMaxChars = (end - p) / 2;  // Quanti wchar_t stanno nel buffer rimanente
size_t descLen = 0;
while (descLen < descMaxChars && descStart[descLen] != L'\0')
    ++descLen;
e.Description = std::wstring(descStart, descLen);
p += (descLen + 1) * 2;  // +1 per il null-terminator, *2 perché ogni wchar_t = 2 byte
```

**Walk del device path:**

Il device path è una sequenza di nodi, ciascuno con il proprio header a 4 byte. Il ciclo avanza di `nodeLen` byte per volta:

```cpp
const uint8_t* dp = dpStart;
while (dp + 4 <= dpEnd) {
    const EFI_DEVICE_PATH_PROTOCOL* node = (const EFI_DEVICE_PATH_PROTOCOL*)dp;
    uint16_t nodeLen = node->Length;
    
    if (node->Type == END_DEVICE_PATH_TYPE) break;
    
    if (node->Type == MEDIA_DEVICE_PATH && node->SubType == MEDIA_HARDDRIVE_DP && nodeLen == 42) {
        // È un nodo HardDrive — estrae il GUID e le coordinate della partizione
        const HARDDRIVE_DEVICE_PATH* hd = (const HARDDRIVE_DEVICE_PATH*)dp;
        e.HD_PartitionNumber = hd->PartitionNumber;
        e.HD_PartitionStart  = hd->PartitionStart;
        // ...
        memcpy(&e.HD_PartitionGUID, hd->Signature, 16);
    }
    
    if (node->Type == MEDIA_DEVICE_PATH && node->SubType == MEDIA_FILEPATH_DP) {
        // È un nodo FilePath — estrae il percorso del bootloader
        const wchar_t* pathStr = (const wchar_t*)(dp + 4);
        // ...
        e.FilePath = std::wstring(pathStr, pathLen);
    }
    
    dp += nodeLen;  // Avanza al prossimo nodo
}
```

#### Funzione `BuildDevicePath(...)`

```
Parametri:
  partitionGUID     — UniquePartitionGUID della partizione ESP
  partitionStartLBA — primo settore della partizione
  partitionSizeLBA  — numero di settori della partizione
  partitionNumber   — numero partizione (1-based)
  filePath          — percorso relativo, es. L"\\EFI\\ubuntu\\grubx64.efi"
Ritorna: std::vector<uint8_t> — blob binario del device path
```

Costruisce sequenzialmente i tre nodi:

**Nodo 1 — HARDDRIVE (42 byte):**
```cpp
HARDDRIVE_DEVICE_PATH hd = {};
hd.Header.Type    = 0x04;  // MEDIA_DEVICE_PATH
hd.Header.SubType = 0x01;  // MEDIA_HARDDRIVE_DP
hd.Header.Length  = 42;
hd.PartitionNumber = partitionNumber;
hd.PartitionStart  = partitionStartLBA;
hd.PartitionSize   = partitionSizeLBA;
hd.MBRType         = 0x02;  // GPT
hd.SignatureType    = 0x02;  // GUID
memcpy(hd.Signature, &partitionGUID, 16);
// Aggiunge tutti i 42 byte al buffer
```

**Nodo 2 — FILEPATH (4 + lunghezza_path byte):**
```cpp
// Normalizza: / → \, aggiunge \ iniziale se assente
size_t pathBytes = (path.size() + 1) * sizeof(wchar_t);  // +1 null, *2 per UTF-16
uint16_t nodeLen = (uint16_t)(4 + pathBytes);             // 4 = header
buf.push_back(0x04);  // Type
buf.push_back(0x04);  // SubType (FilePath)
buf.push_back(nodeLen & 0xFF);  // Length basso
buf.push_back(nodeLen >> 8);    // Length alto
// Poi i byte della stringa UTF-16LE (incluso null-terminator)
```

**Nodo 3 — END (4 byte):**
```cpp
buf.push_back(0x7F);  // END_DEVICE_PATH_TYPE
buf.push_back(0xFF);  // END_ENTIRE_DEVICE_PATH
buf.push_back(4);     // Length
buf.push_back(0);
```

#### Funzione `SerializeBootEntry(const EFIBootEntry& entry)`

Costruisce il blob binario completo di un `EFI_LOAD_OPTION` da scrivere nella NVRAM:

```
Byte 0-3:   Attributes (uint32, little-endian)
Byte 4-5:   FilePathListLength (uint16, lunghezza di RawDevicePath)
Byte 6-N:   Description in UTF-16LE (incluso null-terminator \0\0)
Byte N+1..: RawDevicePath (i byte costruiti da BuildDevicePath)
```

#### Funzione `GetExistingBootNums()`

Scansiona da `Boot0000` a `BootFFFF` per trovare tutte le variabili esistenti. Per ogni numero, fa una "probe" con buffer nullo:

```cpp
DWORD r = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR, nullptr, 0);
DWORD err = GetLastError();
if (r > 0 || err == ERROR_INSUFFICIENT_BUFFER)
    result.push_back((uint16_t)i);
```

Se `err == ERROR_INSUFFICIENT_BUFFER` → la variabile esiste (il buffer era troppo piccolo).  
Se `err == ERROR_ENVVAR_NOT_FOUND` → la variabile non esiste → salta.

**Nota:** questa operazione può essere lenta su alcuni firmware (fino a 65536 query). Nella pratica, la maggior parte dei firmware UEFI ha meno di 20-30 voci di boot e la funzione termina rapidamente.

#### Funzione `FindFreeBootSlot()`

```
Ritorna: uint16_t — il primo numero non usato (0x0000, 0x0001, ...)
```

Chiama `GetExistingBootNums()`, ordina la lista, poi cerca il primo "buco":

```cpp
auto existing = GetExistingBootNums();
std::sort(existing.begin(), existing.end());
for (int i = 0; i <= 0xFFFF; ++i)
    if (!std::binary_search(existing.begin(), existing.end(), (uint16_t)i))
        return (uint16_t)i;
```

`std::binary_search` usa ricerca binaria (O(log n)) sulla lista ordinata.

---

### 4.5 `esp_scanner.h / esp_scanner.cpp` — Scanner partizioni ESP

Questo modulo risolve un problema pratico: la partizione ESP di Linux potrebbe **non avere una lettera di unità** assegnata in Windows (a differenza di C:, D:, ecc.). Dobbiamo trovare il percorso per accedere ai file.

#### Struttura `MountedESP`

```cpp
struct MountedESP {
    std::wstring  VolumePath;      // Es. L"\\?\Volume{1a2b3c...}\" — sempre disponibile
    std::wstring  DriveLetter;     // Es. L"S:\\" — solo se il volume ha una lettera
    std::wstring  AccessPath;      // Il percorso migliore per accedere ai file
    PartitionInfo Partition;       // Dati GPT della partizione
    bool          WasMountedByUs;  // True se abbiamo assegnato noi la lettera
};
```

#### Struttura locale `VolumeInfo`

```cpp
struct VolumeInfo {
    std::wstring VolumePath;    // GUID path volume: \\?\Volume{...}\
    std::wstring DriveLetter;   // Es. "C:\" o vuoto
    int          DiskNumber;    // Disco fisico
    UINT64       StartingOffset; // Offset in byte (per confronto con PartitionInfo)
    UINT64       Length;        // Dimensione
};
```

#### Funzione statica `EnumerateVolumes()`

Usa l'API di Windows per iterare tutti i volumi montati:

```cpp
wchar_t volName[MAX_PATH] = {};
HANDLE hFind = FindFirstVolumeW(volName, MAX_PATH);
// volName ora = L"\\?\Volume{8be4df61-93ca-11d2-aa0d-00e098032b8c}\"
do {
    // 1. Ottieni lettera di unità (se presente)
    GetVolumePathNamesForVolumeNameW(volName, paths, 512, &ret);
    
    // 2. Apri il volume come device
    HANDLE hVol = CreateFileW(devPath, 0, FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    
    // 3. Ottieni su quale disco fisico è e a che offset
    DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, ...);
    
} while (FindNextVolumeW(hFind, volName, MAX_PATH));
```

`IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` restituisce `VOLUME_DISK_EXTENTS`:
```
NumberOfDiskExtents:   1 (per volumi su disco singolo)
Extents[0].DiskNumber: es. 1 (PhysicalDrive1)
Extents[0].StartingOffset: es. 1048576 (= 0x100000 = settore 2048 × 512)
Extents[0].ExtentLength: es. 268435456 (= 256 MB)
```

#### Abbinamento volume ↔ partizione GPT

In `FindAndMountESPs()`, ogni ESP viene confrontata con i volumi enumerati:

```cpp
if (vi.DiskNumber == esp.DiskIndex &&
    vi.StartingOffset == esp.StartingOffsetBytes)
{
    // Trovato! Questo volume corrisponde a questa partizione ESP.
    me.VolumePath  = vi.VolumePath;
    me.DriveLetter = vi.DriveLetter;
    me.AccessPath  = vi.DriveLetter.empty() ? vi.VolumePath : vi.DriveLetter;
}
```

Se il volume non ha una lettera di unità, si usa il **Volume GUID path** direttamente per accedere ai file:
- Con lettera: `S:\EFI\ubuntu\grubx64.efi`
- Senza lettera: `\\?\Volume{1a2b...}\EFI\ubuntu\grubx64.efi`

Entrambe le forme funzionano con le API di Windows (`CreateFileW`, `GetFileAttributesW`, ecc.).

#### Montaggio di emergenza via `SetVolumeMountPointW`

Se l'ESP non è accessibile in nessun modo, il codice assegna dinamicamente una lettera libera:

```cpp
wchar_t letter = FindFreeDriveLetter();  // Es. 'B'
wchar_t mountPoint[4] = { letter, L':', L'\\', L'\0' };  // "B:\"
SetVolumeMountPointW(mountPoint, volumePath);  // Monta il volume su B:
```

Dopo l'uso, `ReleaseDriveLetter()` chiama `DeleteVolumeMountPointW()` per rimuovere il mount point.

---

### 4.6 `os_detector.h / os_detector.cpp` — Rilevamento OS

#### Database `KNOWN_BOOTLOADERS[]`

Un array statico di strutture `KnownBootloader` con percorsi noti per ogni distro:

```cpp
struct KnownBootloader {
    const wchar_t* Path;      // Percorso relativo nell'ESP (lowercased)
    const wchar_t* OSName;    // Nome da mostrare all'utente
    const wchar_t* Vendor;    // Azienda/community
    int            Priority;  // Priorità suggerita (10=Windows, 20=Linux, 50=fallback)
};
```

**Esempi dal database:**
```cpp
{ L"\\efi\\microsoft\\boot\\bootmgfw.efi", L"Windows Boot Manager", L"Microsoft", 10 },
{ L"\\efi\\ubuntu\\grubx64.efi",           L"Ubuntu",               L"Canonical", 20 },
{ L"\\efi\\ubuntu\\shimx64.efi",           L"Ubuntu (Secure Boot)", L"Canonical", 20 },
{ L"\\efi\\fedora\\grubx64.efi",           L"Fedora",               L"Red Hat",   20 },
{ L"\\efi\\refind\\refind_x64.efi",        L"rEFInd Boot Manager",  L"rEFInd",    15 },
{ L"\\efi\\boot\\bootx64.efi",             L"Generic EFI Bootloader", L"Unknown", 50 },
```

Il database comprende 40+ entry per: Ubuntu, Debian, Linux Mint, Pop!_OS, elementary OS, Fedora, CentOS, RHEL, AlmaLinux, Rocky, openSUSE, SLES, Arch, EndeavourOS, Manjaro, Garuda, Gentoo, NixOS, Void, Kali, systemd-boot, rEFInd, OpenCore, Clover, FreeBSD, OpenBSD, NetBSD, macOS.

#### Funzione `DetectOSesInESP(const MountedESP& esp)`

Per ogni voce del database, costruisce il percorso completo e chiama `GetFileAttributesW`:

```cpp
std::wstring base = esp.AccessPath;  // Es. "S:\" o "\\?\Volume{...}\"
// Rimuove il \ finale per concatenazione
if (!base.empty() && base.back() == L'\\') base.pop_back();

for (int i = 0; i < KNOWN_BOOTLOADERS_COUNT; ++i) {
    std::wstring fullPath = base + kb.Path;
    // Es: "S:\\efi\\ubuntu\\grubx64.efi"
    // o:  "\\?\Volume{...}\\efi\\ubuntu\\grubx64.efi"
    
    DWORD attr = GetFileAttributesW(fullPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // File trovato! Aggiungi alla lista.
    }
}
```

#### Funzione `DetectAllOSes(const std::vector<MountedESP>& esps)`

Chiama `DetectOSesInESP` per ogni ESP e poi ordina per `Priority`:

```cpp
std::sort(all.begin(), all.end(),
    [](const DetectedOS& a, const DetectedOS& b) {
        return a.Priority < b.Priority;
    });
```

Windows (priority 10) viene sempre prima di Linux (priority 20), che viene prima di fallback (priority 50).

---

### 4.7 `boot_editor.h / boot_editor.cpp` — Editor voci di boot

#### Struttura `BootEntryFull`

```cpp
struct BootEntryFull {
    EFIBootEntry Entry;      // Tutti i dati parsati dalla variabile UEFI
    int          OrderIndex; // Posizione in BootOrder (0-based; -1 = non in BootOrder)
    bool         IsActive;   // true se LOAD_OPTION_ACTIVE è impostato
    std::wstring DiskLabel;  // Futuro uso (attualmente non popolato)
};
```

#### Funzione `ListAllBootEntries()`

```
Ritorna: std::vector<BootEntryFull> — tutte le voci, in ordine di priorità
```

1. `GetBootOrder()` → lista ordinata di slot (es. `[1, 2, 0]`)
2. Per ogni slot nell'ordine: `ReadBootEntry(num)` → aggiunge alla lista con `OrderIndex` corretto
3. `GetExistingBootNums()` → lista di tutti i Boot variabili esistenti
4. Trova i "orfani" (esistono come `BootXXXX` ma non sono nel `BootOrder`) e li aggiunge con `OrderIndex = -1`

#### Funzione `PrintBootEntries(const std::vector<BootEntryFull>& entries)`

Stampa una tabella formattata:
```
  Ordine Boot#    Att  Nome                             File
  ──────────────────────────────────────────────────────────────────────────────────────────
  1      0001     [*]  Windows Boot Manager             \EFI\Microsoft\Boot\bootmgfw.efi
  2      0002     [*]  Ubuntu                           \EFI\ubuntu\grubx64.efi
  (orfano) 0003   [ ]  Old Entry                        (non-file)
```

- `[*]` = voce attiva (`LOAD_OPTION_ACTIVE` impostato)
- `[ ]` = voce disabilitata
- `(orfano)` = la voce esiste nella NVRAM ma non è nel `BootOrder`

#### Funzione `AddBootEntry(const DetectedOS& os)`

```
Parametri: os — OS rilevato da DetectAllOSes()
Ritorna: uint16_t — numero slot assegnato (es. 0x0002), o 0xFFFF se errore
```

**Sequenza operazioni:**

1. `FindFreeBootSlot()` → slot libero (es. `0x0002`)
2. Popola `EFIBootEntry entry`:
   ```cpp
   entry.BootNum     = slot;
   entry.Attributes  = LOAD_OPTION_ACTIVE;   // = 0x00000001
   entry.Description = os.Name;               // Es. L"Ubuntu"
   entry.Valid       = true;
   entry.HasHDPath   = true;
   ```
3. `BuildDevicePath(os.ESPPartition.UniquePartitionGUID, ...)` → blob device path
4. `WriteBootEntry(slot, entry)` → scrive `Boot0002` nella NVRAM
5. Rilegge `GetBootOrder()` → inserisce `slot` in testa
6. `SetBootOrder(newOrder)` → scrive il nuovo `BootOrder`
7. Se `SetBootOrder` fallisce → `DeleteBootEntry(slot)` (rollback) e ritorna `0xFFFF`

#### Funzione `RemoveBootEntry(uint16_t bootNum)`

1. `DeleteBootEntry(bootNum)` → elimina la variabile `BootXXXX` dalla NVRAM
2. `GetBootOrder()` → legge l'ordine corrente
3. `order.erase(std::remove(...))` → rimuove `bootNum` dalla lista (pattern erase-remove)
4. `SetBootOrder(order)` → scrive il nuovo ordine

#### Funzione `MoveEntryUp(uint16_t bootNum)` / `MoveEntryDown(uint16_t bootNum)`

```cpp
auto order = GetBootOrder();  // Es. [1, 2, 3]
auto it = std::find(order.begin(), order.end(), bootNum);  // Cerca 2 → it punta a index 1
// MoveEntryUp:
std::swap(*it, *std::prev(it));  // Swap con elemento precedente → [2, 1, 3]
SetBootOrder(order);
```

#### Funzione `SetBootNext(uint16_t bootNum)`

Scrive la variabile `BootNext` — un `uint16_t` in little-endian (2 byte):

```cpp
uint8_t data[2] = { (uint8_t)(bootNum & 0xFF), (uint8_t)(bootNum >> 8) };
SetFirmwareEnvironmentVariableW(L"BootNext", EFI_GLOBAL_GUID_STR, data, 2);
```

`BootNext` viene letto dal firmware **una sola volta** al prossimo avvio e poi cancellato automaticamente.

#### Funzione `SetEntryActive(uint16_t bootNum, bool active)`

Legge la voce, modifica solo il bit `LOAD_OPTION_ACTIVE`, riscrive:

```cpp
auto entry = ReadBootEntry(bootNum);
if (active)
    entry.Attributes |=  LOAD_OPTION_ACTIVE;   // Set bit 0
else
    entry.Attributes &= ~LOAD_OPTION_ACTIVE;   // Clear bit 0
WriteBootEntry(bootNum, entry);
```

#### Funzione `AutoConfigure()`

**Sequenza completa:**

1. `FindAllESPs()` → lista di tutte le ESP su tutti i dischi
2. `FindAndMountESPs(esps)` → monta le ESP non accessibili
3. `DetectAllOSes(mounted)` → lista OS rilevati, ordinata per priorità
4. `ListAllBootEntries()` → voci già presenti nella NVRAM
5. Per ogni OS rilevato:
   - Controlla se esiste già una voce con lo stesso `UniquePartitionGUID` e lo stesso `FilePath`
   - Se non esiste → `AddBootEntry(os)` → aggiunge la voce
6. Riordina `BootOrder`: mette le voci Windows (il cui `Description` contiene "windows", case-insensitive) prima di tutte le altre con `std::stable_sort`
7. Smonta le ESP montate da noi: `ReleaseDriveLetter(m.DriveLetter)`

**Confronto per evitare duplicati:**
```cpp
bool samePartition = ex.Entry.HasHDPath &&
    ex.Entry.HD_PartitionGUID == os.ESPPartition.UniquePartitionGUID;
bool samePath = ToLower(ex.Entry.FilePath) == ToLower(os.BootloaderPath);
if (samePartition && samePath) alreadyPresent = true;
```

Il confronto è case-insensitive sul percorso perché alcuni firmware cambiano le maiuscole.

---

### 4.8 `backup.h / backup.cpp` — Backup e ripristino

#### Struttura `UEFISnapshot`

```cpp
struct UEFISnapshot {
    std::vector<uint16_t>                           BootOrder;      // Lista slot
    std::vector<std::pair<uint16_t,
                          std::vector<uint8_t>>>    BootEntries;    // num → blob raw
    std::wstring                                    Timestamp;      // "2026-06-26_22-43-00"
    std::wstring                                    Note;           // Nota utente
};
```

`BootEntries` è una lista di coppie `(uint16_t BootNum, vector<uint8_t> RawBlob)`.  
Il `RawBlob` è il contenuto **esatto** della variabile UEFI — non il blob parsato, ma i byte grezzi letti da `GetFirmwareEnvironmentVariableW`.

Questo garantisce che il ripristino sia **perfettamente fedele**: anche variabili con campi non standard o dati opzionali vengono preservati.

#### Formato file `.bcdtool`

```
Offset  Dimensione  Campo
──────  ──────────  ──────────────────────────────────────────────────────
     0          13  Magic: "BCDTOOL_SNAP\0" (13 byte ASCII + null)
    13           4  Version: uint32 = 1 (little-endian)
    17           4  TimestampLen: uint32 (numero di wchar_t incluso il null)
    21        T×2   Timestamp: UTF-16LE (T = TimestampLen)
  17+T           4  NoteLen: uint32
  21+T        N×2   Note: UTF-16LE
21+T+N           4  BootOrderCount: uint32
25+T+N       O×2   BootOrder: array di uint16_t
     ...          4  EntryCount: uint32
     ...     per ogni entry:
                 2  BootNum: uint16
                 4  BlobSize: uint32
              B×1   Blob: byte grezzi della variabile UEFI
```

**Lettura del magic:**
```cpp
char magic[sizeof(SNAP_MAGIC)] = {};
fread(magic, 1, sizeof(SNAP_MAGIC), f);
if (memcmp(magic, SNAP_MAGIC, sizeof(SNAP_MAGIC)) != 0)
    return false;  // File corrotto o non valido
```

#### Funzione `SaveSnapshot(const std::wstring& filePath, const std::wstring& note)`

**Sequenza:**

1. `GetBootOrder()` → salva l'ordine corrente
2. `GetExistingBootNums()` → ottiene tutti i numeri di slot esistenti
3. Per ogni slot: `RawReadBootVar(num)` → legge il blob grezzo dalla NVRAM
4. Apre il file in modalità binaria (`"wb"`)
5. Scrive header, versione, timestamp, nota, BootOrder, poi le entry una per una

**Funzioni helper di scrittura:**
- `WriteU32(FILE* f, uint32_t v)` → `fwrite(&v, 4, 1, f)` — scrive 4 byte
- `WriteU16(FILE* f, uint16_t v)` → `fwrite(&v, 2, 1, f)` — scrive 2 byte
- `WriteWStr(FILE* f, const std::wstring& s)` → scrive prima la lunghezza (uint32), poi i byte UTF-16
- `WriteBytes(FILE* f, const vector<uint8_t>& v)` → scrive prima la dimensione (uint32), poi i byte

#### Funzione `ApplySnapshot(const UEFISnapshot& snap)`

**Sequenza:**

1. `GetExistingBootNums()` → lista di tutti i slot UEFI correnti
2. Per ogni slot corrente: `RawDeleteBootVar(num)` → elimina la variabile dalla NVRAM  
   **Attenzione:** questo cancella TUTTO, incluse voci che non avevamo creato noi
3. Per ogni entry nel backup: `RawWriteBootVar(num, blob)` → riscrive il blob esatto
4. `SetBootOrder(snap.BootOrder)` → ripristina l'ordine originale

**Perché cancellare tutto prima?**

Se aggiungiamo nuovi slot (es. Boot0005) e poi ripristiniamo, quei slot rimarrebbero se non cancellassimo prima. Il ripristino deve essere idempotente: il risultato finale deve essere identico allo stato al momento del backup.

---

### 4.9 `main.cpp` — Interfaccia utente

#### Funzione `wmain(int argc, wchar_t* argv[])`

Punto di ingresso del programma (versione Unicode di `main`):

```cpp
int wmain(int argc, wchar_t* argv[])
{
    // 1. Imposta encoding UTF-8 per il terminale
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);  // stdin/stdout in modalità Unicode

    // 2. Richiede elevazione UAC se necessario (può uscire e rilanciare)
    EnsureAdminOrElevate(argc, argv);

    // 3. Imposta titolo finestra
    SetConsoleTitleW(L"BCDToolWindows – EFI Boot Manager");

    // 4. Acquisisce privilegi (UEFI, disco, filesystem)
    AcquireAllRequiredPrivileges();

    // 5. Verifica che il sistema sia UEFI (non BIOS legacy)
    CheckUEFISupport();

    // 6. Entra nel loop del menu principale
    MainMenu();
}
```

#### Funzione `CheckUEFISupport()`

Prova a leggere `BootOrder` e interpreta il codice di errore:

```cpp
GetFirmwareEnvironmentVariableW(L"BootOrder", L"{8BE4DF61-...}", nullptr, 0);
DWORD err = GetLastError();
// ERROR_INVALID_FUNCTION (0x1) → sistema avviato in Legacy BIOS → errore fatale
// ERROR_ACCESS_DENIED (0x5)   → mancano i privilegi → errore fatale  
// ERROR_INSUFFICIENT_BUFFER   → variabile esiste, UEFI ok → continua
```

#### Variabile `snapshotSaved` in `MenuAddEntry()` e `MenuRemoveEntry()`

```cpp
static bool snapshotSaved = false;
```

Variabile statica locale: viene inizializzata a `false` una sola volta per tutta la sessione. Garantisce che il backup automatico venga creato **solo la prima volta** che l'utente fa una modifica — non ad ogni aggiunta.

---

## 5. Catene di chiamata per ogni voce di menu

### Menu 1: "Visualizza voci di avvio correnti"

```
MenuListEntries()
  └── ListAllBootEntries()
        ├── GetBootOrder()
        │     └── ReadUEFIVar(L"BootOrder")
        │           └── GetFirmwareEnvironmentVariableW("BootOrder", ...)
        ├── GetExistingBootNums()
        │     ├── GetBootOrder() → slot primari (sempre presenti)
        │     └── [loop 0..0x01FF, max 32 miss consecutive] probe firmware
        └── [per ogni slot] ReadBootEntry(num)
              ├── swprintf_s → L"Boot0001"
              ├── ReadUEFIVar(L"Boot0001")
              │     └── GetFirmwareEnvironmentVariableW(...)
              └── ParseLoadOption(num, raw)
                    ├── Legge Attributes (4 byte)
                    ├── Legge FilePathListLength (2 byte)
                    ├── Legge Description (UTF-16, fino a \0\0)
                    ├── Copia RawDevicePath
                    └── [loop nodi] Estrae HD e FilePath
  └── PrintBootEntries(entries)
        └── [per ogni entry] wprintf(tabella formattata)
  └── GetBootOrder() [di nuovo, per stampare l'ordine]
```

### Menu 2: "Scansiona dischi"

```
MenuScanDisks()
  ├── EnumerateDisks()
  │     └── [loop 0..31] ReadDiskLayout(i, di)
  │           ├── OpenDisk(i)          → CreateFileW("\\.\PhysicalDrive%d")
  │           ├── GetBytesPerSector()  → DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)
  │           ├── GetDiskSize()        → DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY_EX)
  │           └── DeviceIoControl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX)
  │                 → riempie DRIVE_LAYOUT_INFORMATION_EX
  │                 → loop partizioni → popola vector<PartitionInfo>
  ├── FindAllESPs()
  │     └── EnumerateDisks() [di nuovo]
  │           └── filtra per IsESP == true
  ├── FindAndMountESPs(esps)
  │     ├── EnumerateVolumes()
  │     │     ├── FindFirstVolumeW / FindNextVolumeW
  │     │     ├── GetVolumePathNamesForVolumeNameW (per lettera di unità)
  │     │     └── DeviceIoControl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)
  │     └── [per ogni ESP] abbina a volume → popola MountedESP
  └── DetectAllOSes(mounted)
        └── [per ogni ESP] DetectOSesInESP(esp)
              └── [per ogni entry DB] GetFileAttributesW(fullPath)
```

### Menu 3: "Aggiungi voce di avvio"

```
MenuAddEntry()
  ├── FindAllESPs() + FindAndMountESPs() + DetectAllOSes()
  │     (vedi Menu 2)
  ├── ListAllBootEntries()
  │     (vedi Menu 1)
  ├── [filtra OS senza voce esistente]
  ├── [utente sceglie numero e opzionalmente modifica nome]
  ├── [prima volta] SaveSnapshot(DefaultSnapshotPath(), ...)
  │     ├── GetBootOrder()
  │     ├── GetExistingBootNums()
  │     ├── [per ogni slot] RawReadBootVar(num)
  │     │     └── GetFirmwareEnvironmentVariableW(...)
  │     └── fwrite → file .bcdtool
  └── AddBootEntry(osToAdd)
        ├── FindFreeBootSlot()
        │     ├── GetExistingBootNums()
        │     └── std::binary_search (cerca primo buco)
        ├── BuildDevicePath(GUID, startLBA, sizeLBA, partNum, filePath)
        │     ├── Costruisce HARDDRIVE_DEVICE_PATH (42 byte)
        │     ├── Costruisce FILEPATH node (4 + pathBytes)
        │     └── Aggiunge END node (4 byte)
        ├── WriteBootEntry(slot, entry)
        │     ├── SerializeBootEntry()
        │     │     ├── Serializza Attributes (4 byte LE)
        │     │     ├── Serializza FilePathListLength (2 byte LE)
        │     │     ├── Serializza Description (UTF-16LE + \0\0)
        │     │     └── Appende RawDevicePath
        │     └── WriteUEFIVar(L"Boot0002", blob)
        │           └── SetFirmwareEnvironmentVariableW(...)
        ├── GetBootOrder()
        ├── order.insert(order.begin(), slot)  // prepend
        └── SetBootOrder(newOrder)
              ├── [serializza in little-endian]
              └── WriteUEFIVar(L"BootOrder", ...)
```

### Menu 4: "Rimuovi voce di avvio"

```
MenuRemoveEntry()
  ├── ListAllBootEntries()        (vedi sopra)
  ├── PrintBootEntries()
  ├── [utente inserisce numero hex]
  ├── SaveSnapshot(...)           (backup automatico)
  └── RemoveBootEntry(bootNum)
        ├── DeleteBootEntry(bootNum)
        │     └── DeleteUEFIVar(L"Boot%04X")
        │           └── SetFirmwareEnvironmentVariableW(..., nullptr, 0)
        ├── GetBootOrder()
        ├── order.erase(std::remove(...))  // rimuove bootNum dalla lista
        └── SetBootOrder(order)
```

### Menu 5: "Cambia ordine di avvio"

```
MenuChangeOrder()
  loop:
    ├── ListAllBootEntries() + PrintBootEntries()
    ├── [input: "su 0002" o "giu 0002"]
    └── MoveEntryUp(0x0002) o MoveEntryDown(0x0002)
          ├── GetBootOrder()        → es. [1, 2, 3]
          ├── std::find(...)        → trova posizione di 0x0002 (index 1)
          ├── std::swap(*it, *prev) → swap con [1] e [0] → [2, 1, 3]
          └── SetBootOrder([2,1,3])
```

### Menu 6: "Auto-configura tutto"

```
MenuAutoConfigure()
  ├── SaveSnapshot(...)        (backup obbligatorio prima)
  └── AutoConfigure()
        ├── FindAllESPs()
        ├── FindAndMountESPs()
        ├── DetectAllOSes()    → ordinato per Priority
        ├── ListAllBootEntries()
        ├── [per ogni OS rilevato]
        │     ├── [controlla duplicati: GUID partizione + percorso file]
        │     └── [se mancante] AddBootEntry(os)
        ├── GetBootOrder()
        ├── std::stable_sort   → Windows prima (desc contiene "windows")
        ├── SetBootOrder()
        └── [per ogni ESP montata da noi] ReleaseDriveLetter()
```

### Menu 7 → 1: "Crea backup"

```
MenuBackupRestore() → scelta 1
  ├── [utente inserisce nota]
  └── SaveSnapshot(DefaultSnapshotPath(), nota)
        ├── CurrentTimestamp()        → GetLocalTime() → stringa "AAAA-MM-GG_HH-MM-SS"
        ├── GetBootOrder()            → ReadUEFIVar(L"BootOrder")
        ├── GetExistingBootNums()     → probe 0..FFFF
        ├── [per ogni slot] RawReadBootVar() → GetFirmwareEnvironmentVariableW
        ├── _wfopen_s(filePath, "wb") → apre file binario
        ├── fwrite(SNAP_MAGIC)
        ├── WriteU32(SNAP_VERSION)
        ├── WriteWStr(Timestamp)
        ├── WriteWStr(Note)
        ├── WriteU32(BootOrder.size())
        ├── [per ogni slot] WriteU16(num)
        ├── WriteU32(BootEntries.size())
        └── [per ogni entry] WriteU16(num) + WriteBytes(blob)
```

### Menu 7 → 2: "Ripristina da backup"

```
MenuBackupRestore() → scelta 2
  ├── ListSnapshots()
  │     ├── GetModuleFileNameW()    → percorso exe
  │     └── FindFirstFileW("*.bcdtool") / FindNextFileW
  ├── [per ogni file] LoadSnapshot()
  │     ├── fread(magic) + memcmp   → verifica integrità
  │     ├── ReadU32(version)
  │     ├── ReadWStr(Timestamp)
  │     ├── ReadWStr(Note)
  │     ├── ReadU32(orderCount) + [loop] ReadU16
  │     └── ReadU32(entryCount) + [loop] ReadU16 + ReadBytes
  ├── [utente sceglie]
  ├── SaveSnapshot(...)             (backup dello stato attuale prima del ripristino)
  └── ApplySnapshot(snap)
        ├── GetExistingBootNums()
        ├── [per ogni slot] RawDeleteBootVar()
        │     └── SetFirmwareEnvironmentVariableW(..., nullptr, 0)
        ├── [per ogni entry backup] RawWriteBootVar(num, blob)
        │     └── SetFirmwareEnvironmentVariableW(name, GUID, blob.data(), size)
        └── SetBootOrder(snap.BootOrder)
```

---

## 6. Formato binario delle variabili UEFI

### `BootOrder` — esempio reale

```
Byte  Valore  Significato
────  ──────  ───────────────────────────────────
  0   0x01    Boot0001 (byte basso)
  1   0x00    Boot0001 (byte alto)
  2   0x02    Boot0002 (byte basso)
  3   0x00    Boot0002 (byte alto)
  4   0x00    Boot0000 (byte basso)
  5   0x00    Boot0000 (byte alto)
→ BootOrder = [0x0001, 0x0002, 0x0000]
```

### `Boot0002` — esempio per Ubuntu su secondo disco

Supponiamo:
- Description: "Ubuntu" (6 caratteri UTF-16 = 12 byte + 2 null = 14 byte)
- Partizione ESP: numero 1, LBA 2048, size 532480, GUID `{A1B2C3D4-...}`
- Bootloader: `\EFI\ubuntu\grubx64.efi` (23 char UTF-16 = 46 byte + 2 null = 48 byte)

```
Offset  Hex         Significato
──────  ──────────  ──────────────────────────────────────────────────────
  0-3   01 00 00 00  Attributes: LOAD_OPTION_ACTIVE = 0x00000001
  4-5   36 00        FilePathListLength: 0x0036 = 54 byte
                       (42 nodo HD + 4+48 nodo FilePath + 4 nodo END = 98... )
                       [nota: 42 + 52 + 4 = 98 = 0x62... dipende dal path]
  6-7   55 00        'U' in UTF-16LE
  8-9   62 00        'b'
 10-11  75 00        'u'
 12-13  6E 00        'n'
 14-15  74 00        't'
 16-17  75 00        'u'
 18-19  00 00        null-terminator → fine Description

  ── HARDDRIVE_DEVICE_PATH (42 byte) ────────────────────────────────────
 20     04           Type = MEDIA_DEVICE_PATH
 21     01           SubType = MEDIA_HARDDRIVE_DP
 22-23  2A 00        Length = 42
 24-27  01 00 00 00  PartitionNumber = 1
 28-35  00 08 00 00 00 00 00 00  PartitionStart = LBA 0x0800 = 2048
 36-43  00 20 08 00 00 00 00 00  PartitionSize = LBA 0x082000 = 532480
 44-59  [16 byte UniquePartitionGUID della partizione ESP]
 60     02           MBRType = GPT
 61     02           SignatureType = GUID

  ── FILEPATH_DEVICE_PATH ────────────────────────────────────────────────
 62     04           Type = MEDIA_DEVICE_PATH
 63     04           SubType = MEDIA_FILEPATH_DP
 64-65  36 00        Length = 54 (4 header + 25×2 byte path)
 66-115 [path UTF-16LE: \EFI\ubuntu\grubx64.efi + \0\0]

  ── END_DEVICE_PATH ─────────────────────────────────────────────────────
116     7F           END_DEVICE_PATH_TYPE
117     FF           END_ENTIRE_DEVICE_PATH
118-119 04 00        Length = 4
```

---

## 7. Formato binario del file di backup `.bcdtool`

```
Offset  Dim  Campo
──────  ───  ──────────────────────────────────────────────────────────────
     0   13  Magic: 42 43 44 54 4F 4F 4C 5F 53 4E 41 50 00
             ("BCDTOOL_SNAP\0")
    13    4  Version: 01 00 00 00  (= 1 uint32 LE)
    17    4  TimestampLen: es. 14 00 00 00  (14 wchar_t incluso null)
    21   28  Timestamp UTF-16LE: "2026-06-26_22-43-00\0"
    49    4  NoteLen: es. 0F 00 00 00  (15 wchar_t)
    53   30  Note UTF-16LE: "Backup manuale\0"
    83    4  BootOrderCount: es. 03 00 00 00
    87    6  BootOrder: 01 00  02 00  00 00
    93    4  EntryCount: es. 03 00 00 00
  ────
    97    2  BootNum: 01 00  (= Boot0001)
    99    4  BlobSize: es. 7A 00 00 00  (122 byte)
   103  122  Blob grezzo di Boot0001
  ────
   225    2  BootNum: 02 00  (= Boot0002)
   227    4  BlobSize: es. 8C 00 00 00  (140 byte)
   231  140  Blob grezzo di Boot0002
  ────
   (... altre entry ...)
```

---

## 8. Requisiti, compilazione e utilizzo

### Requisiti di sistema

- Windows 10 / Windows 11 (minimo Windows 8 / Server 2012)
- Firmware **UEFI** (non Legacy BIOS — verificato automaticamente all'avvio)
- Disco con tabella **GPT** (non MBR)
- **Eseguire come Amministratore** (richiesto automaticamente via UAC)

### Compilazione con Visual Studio

```cmd
git clone https://github.com/tuonome/BCDToolWindows.git
cd BCDToolWindows
cmake -B build -G "Visual Studio 17 2022" -A x64 .
cmake --build build --config Release
```

Oppure semplicemente:
```cmd
build.bat
```

### Compilazione con MinGW-w64 (MSYS2)

```bash
# Dal terminale MSYS2 MinGW64:
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
```
Poi dal prompt di Windows:
```cmd
build.bat
```

### Procedura dual boot consigliata

1. Installa Linux su secondo disco. Durante l'installazione, lascia che installi il suo bootloader nella propria partizione ESP — non cambiare le impostazioni del bootloader.
2. Avvia Windows normalmente.
3. Esegui `BCDToolWindows.exe` come Amministratore.
4. Scegli **"6. Auto-configura tutto"** → crea il backup e aggiunge le voci mancanti.
5. Riavvia: il menu del firmware UEFI (o Windows Boot Manager) mostrerà entrambi i sistemi.

### Ripristino completo (rimozione Linux)

**Con backup:**
1. Spegni il PC, rimuovi o tieni il disco Linux (non importa).
2. Avvia Windows (potrebbe avviare direttamente senza menu se il disco Linux è rimosso).
3. Esegui BCDToolWindows → **"7. Backup e ripristino" → "Ripristina da backup"**.
4. Seleziona il backup creato prima di aggiungere Linux.
5. Il firmware UEFI torna esattamente com'era prima.

**Senza backup (rimozione manuale):**
1. Menu **"4. Rimuovi voce"** → inserisci il numero Boot# della voce Linux.
2. Conferma → la voce viene eliminata dalla NVRAM.
3. Windows rimane come unica voce di avvio.

### Avvertenze

> ⚠️ Modificare le variabili UEFI è un'operazione di sistema a basso livello.  
> Il programma crea sempre un backup automatico prima di ogni modifica.  
> In caso di problemi: accedi al BIOS/UEFI della tua scheda madre e usa  
> l'opzione "Boot Override" o "Restore Defaults" per ripristinare manualmente.

---

## 0. Guida rapida: Linux Mint su secondo disco

Questa è la procedura **esatta e sicura** per installare Linux Mint mantenendo Windows intatto, usando BCDToolWindows invece di GRUB come gestore dell'avvio.

### Prima dell'installazione di Linux Mint (in Windows)

1. Crea un backup UEFI con BCDToolWindows:
   ```
   Menu 7 → "Crea backup"
   ```
   Salva il file `.bcdtool` che viene creato — è il tuo punto di ripristino completo.

2. Annota quale slot Boot# corrisponde a Windows (es. `Boot0001`).

### Installazione di Linux Mint

**Scenario A — Stacchi fisicamente il disco Windows durante l'installazione (consigliato)**

Questo è l'approccio più sicuro. L'installer di Linux non "vede" il disco Windows, quindi non può toccarlo per nessun motivo.

1. Spegni il PC
2. **Stacca fisicamente il disco con Windows** (cavo SATA o M.2)
3. Avvia da USB con Linux Mint
4. Installa normalmente — l'installer vedrà solo il secondo disco
5. **Alla domanda sul bootloader**: lascia le impostazioni di default
   - Linux Mint installerà GRUB sulla **propria ESP** (partizione EFI del secondo disco)
   - Non tocca niente del disco Windows perché il disco Windows non è connesso
6. Termina l'installazione
7. Spegni il PC, **riconnetti il disco Windows**
8. Avvia Windows (il firmware UEFI probabilmente avvierà l'ultimo OS installato — se va su Linux, usa il tasto `F8`/`F12` per scegliere Windows)
9. Avvia BCDToolWindows come Amministratore → **Menu 6 "Auto-configura tutto"**
10. Il programma trova sia Windows sia Linux Mint nelle rispettive ESP e aggiunge entrambe le voci nella NVRAM
11. Riavvia: d'ora in poi il firmware mostra le opzioni (o va su Windows di default)

**Scenario B — Tieni entrambi i dischi connessi durante l'installazione**

Funziona anche così, ma richiede più attenzione nella selezione del disco durante l'installazione.

1. Avvia da USB con Linux Mint
2. All'installer, scegli **"Installazione personalizzata"** (non automatica)
3. Assicurati di selezionare il **secondo disco** come destinazione
4. Nella sezione "Dispositivo per il bootloader" (o simile): **seleziona la partizione EFI del secondo disco** (non quella di Windows)
   - In Linux Mint: il bootloader si configura automaticamente nella partizione EFI del disco selezionato
5. Procedi con l'installazione
6. Al riavvio: avvia Windows, poi BCDToolWindows → **Menu 6**

### Cosa si installa nella ESP di Linux Mint?

Linux Mint usa GRUB come bootloader. Nella sua partizione ESP crea:
```
/EFI/linuxmint/grubx64.efi     ← bootloader principale
/EFI/linuxmint/shimx64.efi     ← shim per Secure Boot (se abilitato)
```

BCDToolWindows rileva automaticamente entrambi (sono nel database di `os_detector.cpp`) e crea:
```
Boot0002: "Linux Mint"  →  HD(1,GPT,{GUID-ESP-Linux},...)\EFI\linuxmint\grubx64.efi
```

### Cosa succede se stacchi il disco Linux dopo?

```
Avvio PC
  ├── Firmware legge BootOrder: [Boot0001=Windows, Boot0002=Linux Mint]
  ├── Tenta Boot0002: cerca partizione con GUID {GUID-ESP-Linux}
  │     → Disco non trovato → nessuna partizione con quel GUID
  │     → Salta automaticamente
  └── Tenta Boot0001: cerca partizione con GUID {GUID-ESP-Windows}
        → Trovata sul disco Windows → avvia Windows normalmente
```

Nessun errore, nessun intervento. Il firmware gestisce l'assenza del disco da solo.

### Cosa succede se poi riattacchi il disco Linux?

Il firmware trova di nuovo la partizione ESP di Linux → Boot0002 funziona di nuovo. Il menu di avvio torna a mostrare entrambe le opzioni. **Nessuna riconfigurazione necessaria.**

---

## 8. Compatibilità OS e sicurezza

### Sistema operativo Windows richiesto

| Versione Windows | Supporto |
|---|---|
| Windows 11 (qualsiasi versione) | ✅ Confermato compatibile |
| Windows 10 (build 1903 e successive) | ✅ Confermato compatibile |
| Windows 10 (build precedenti) | ✅ Dovrebbe funzionare (stessa API) |
| Windows 8.1 | ⚠️ Probabilmente funziona (API presente) — non testato |
| Windows 8 | ⚠️ Probabilmente funziona — non testato |
| Windows 7 | ❌ Non supportato — `GetFirmwareEnvironmentVariableW` non gestisce UEFI vars |
| Windows Vista / XP | ❌ Non supportato |

> Le API `GetFirmwareEnvironmentVariableW` / `SetFirmwareEnvironmentVariableW` con supporto completo per UEFI (non solo Legacy) sono garantite da Windows 8 in poi. Su Windows 7, queste funzioni esistono ma operano solo su variabili di tipo diverso.

### Firmware UEFI richiesto

| Tipo firmware | Supporto |
|---|---|
| UEFI 2.x (2009+) — schede madre moderne | ✅ Pienamente compatibile |
| UEFI 1.x (2006-2008) — schede madre vecchie | ⚠️ Probabilmente funziona — non testato |
| Legacy BIOS (qualsiasi anno) | ❌ Non funziona — rilevato all'avvio |
| Dual-mode UEFI/BIOS in modalità BIOS | ❌ Verificare nelle impostazioni BIOS che sia in modalità UEFI |

### Disco richiesto

| Tipo tabella partizioni | Supporto |
|---|---|
| GPT (GUID Partition Table) | ✅ Richiesto per UEFI boot |
| MBR (Master Boot Record) | ❌ Non supportato per le operazioni di boot UEFI (il disco MBR viene ignorato) |

> Attenzione: Windows 11 richiede GPT per la partizione di sistema. Se hai Windows 11, sicuramente hai GPT.

### Cosa fa e cosa NON fa questo programma

#### Cosa fa (operazioni sicure)

| Operazione | Effetto |
|---|---|
| **Legge** variabili UEFI | Solo lettura dalla NVRAM — nessun danno possibile |
| **Legge** dischi GPT | Solo lettura (IOCTL read-only) — nessun danno possibile |
| **Aggiunge** voci Boot | Scrive una nuova variabile UEFI — non tocca i file sui dischi |
| **Modifica** BootOrder | Cambia l'ordine di priorità — non tocca i file |
| **Elimina** voci Boot | Rimuove la voce dalla NVRAM — non tocca i file |
| **Monta** ESP | Assegna una lettera di unità — reversibile |
| **Crea** backup | Scrive un file `.bcdtool` su disco — nessun effetto sul firmware |
| **Ripristina** backup | Sovrascrive variabili UEFI con i valori salvati |

#### Cosa NON fa (mai)

- ❌ Non modifica i file su disco (bootmgfw.efi, grubx64.efi, ecc.)
- ❌ Non modifica il BCD store di Windows (`C:\Boot\BCD`)
- ❌ Non formatta partizioni
- ❌ Non scrive sul MBR o sull'inizio dei dischi
- ❌ Non installa/disinstalla bootloader
- ❌ Non modifica file di configurazione di Linux (fstab, grub.cfg, ecc.)
- ❌ Non ha accesso a Internet
- ❌ Non installa driver

### Analisi dei rischi per operazione

#### Rischio BASSO — lettura e visualizzazione

Funzioni: `ListAllBootEntries`, `MenuListEntries`, `MenuScanDisks`, `GetBootOrder`, `ReadBootEntry`, `EnumerateDisks`

Nessun rischio. Sono operazioni di sola lettura. Non modificano nulla.

#### Rischio BASSO — aggiunta voce

Funzione: `AddBootEntry`, `MenuAddEntry`

Aggiunge una nuova variabile `BootXXXX` e aggiorna `BootOrder`. Il caso peggiore è una voce mal formata che il firmware ignora — Windows continua ad avviarsi normalmente dalla voce precedente.

**Protezione:** backup automatico creato prima della prima modifica della sessione.

#### Rischio MEDIO — rimozione voce

Funzione: `RemoveBootEntry`, `MenuRemoveEntry`

Elimina una variabile `BootXXXX` e aggiorna `BootOrder`. Se elimini per errore la voce di Windows, al prossimo avvio il firmware potrebbe mostrare un menu di selezione manuale (F8/F12) oppure tentare una voce fallback.

**Protezione:** 
- Conferma richiesta prima di eliminare
- Backup automatico creato prima della rimozione
- Il firmware ha sempre una fallback: legge il file `\EFI\BOOT\BOOTx64.EFI` da qualsiasi ESP se non trova voci valide

#### Rischio ALTO — ripristino snapshot

Funzione: `ApplySnapshot`, `MenuBackupRestore → scelta 2`

Elimina **tutte** le voci UEFI correnti e le riscrive dal backup. Se il backup è corrotto o il file `.bcdtool` è stato copiato da un altro PC, si potrebbe finire con voci non valide.

**Protezione:**
- Prima del ripristino viene creato automaticamente un backup dello stato corrente
- Il firmware ha sempre il fallback `\EFI\BOOT\BOOTx64.EFI`
- Il file `.bcdtool` contiene un magic number verificato prima del ripristino

#### Cosa fare se Windows non parte dopo un errore

**Scenario 1:** Hai usato BCDToolWindows e al prossimo avvio non parte nulla.
- Premi `F8`, `F11`, `F12`, `DEL` o `ESC` durante il POST (dipende dalla scheda madre) per aprire il menu di avvio manuale del firmware
- Cerca la voce "Windows Boot Manager" o "EFI\Microsoft\Boot\bootmgfw.efi"
- Se la trovi: avvia → poi usa BCDToolWindows per correggere le voci

**Scenario 2:** Il firmware non trova nessuna voce valida e mostra "No bootable device".
- La ESP di Windows contiene ancora `\EFI\BOOT\BOOTx64.EFI` (copia del Windows Boot Manager)
- Dalla shell EFI (se disponibile): `\EFI\Microsoft\Boot\bootmgfw.efi`
- Oppure: avvia da USB Windows → Ripristino → Prompt dei comandi → `bcdedit /set {default} device partition=C:` (poi BCDToolWindows per sistemare le voci UEFI)

**Scenario 3:** Hai un file di backup `.bcdtool`.
- Avvia Windows con qualsiasi metodo
- BCDToolWindows → Menu 7 → Ripristina → seleziona il backup

### Bug risolti prima del rilascio

Questi problemi sono stati identificati e corretti nel codice:

| Bug | File | Fix applicato |
|---|---|---|
| `wcsicmp` non esiste in MSVC | `main.cpp` | Sostituito con `_wcsicmp` via `compat.h` |
| `_getws_s` non esiste in MinGW | `main.cpp` | Sostituito con `ReadWideLine()` (usa `fgetws`) |
| `swscanf_s` incompatibile con MinGW | `main.cpp` | Sostituito con `ScanWideTwo()` con ifdef |
| `GetExistingBootNums` scansionava 65536 variabili | `uefi_vars.cpp` | Limitato a 512 slot con early-exit dopo 32 miss |
| Divisione per zero se `BytesPerSector == 0` | `gpt_reader.cpp` | Aggiunto guard con fallback a 512 |

---

## 9. Requisiti, compilazione e utilizzo

### Requisiti di sistema

- Windows 10 / Windows 11 — **pienamente testato**
- Windows 8 / 8.1 — probabilmente funziona (non testato)
- Firmware **UEFI** (non Legacy BIOS — verificato automaticamente all'avvio)
- Disco con tabella **GPT** (non MBR)
- **Eseguire come Amministratore** (richiesto automaticamente via UAC)

### Compilazione con Visual Studio

```cmd
git clone https://github.com/tuonome/BCDToolWindows.git
cd BCDToolWindows
cmake -B build -G "Visual Studio 17 2022" -A x64 .
cmake --build build --config Release
```

Oppure semplicemente:
```cmd
build.bat
```

### Compilazione con MinGW-w64 (MSYS2)

```bash
# Dal terminale MSYS2 MinGW64:
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
```
Poi dal prompt di Windows:
```cmd
build.bat
```

### Procedura dual boot (con BCDToolWindows)

Vedi la sezione [0. Guida rapida: Linux Mint su secondo disco](#0-guida-rapida-linux-mint-su-secondo-disco) per la procedura completa e dettagliata.

### Ripristino completo (rimozione Linux)

**Con backup (modo sicuro):**
1. Spegni il PC, rimuovi o tieni il disco Linux (non importa).
2. Avvia Windows (il firmware salta automaticamente la voce Linux se il disco è assente).
3. Esegui BCDToolWindows → **"7. Backup e ripristino" → "Ripristina da backup"**.
4. Seleziona il backup creato prima di aggiungere Linux.
5. Il firmware UEFI torna esattamente com'era prima.

**Senza backup (rimozione manuale):**
1. Menu **"4. Rimuovi voce"** → inserisci il numero Boot# della voce Linux.
2. Conferma → la voce viene eliminata dalla NVRAM.
3. Windows rimane come unica voce di avvio.

---

## Licenza

MIT — vedi [LICENSE](LICENSE).
