# BCDToolWindows — EFI Boot Manager per Windows

> Gestione avanzata delle voci di avvio UEFI direttamente da Windows, senza dipendenze da Linux o GRUB.

---

## Perché questo progetto?

Quando installi Linux su un secondo disco mantenendo Windows sul primo, il modo classico è installare GRUB.  
Problema: se stacchi il disco Linux, GRUB non trova i propri file e il sistema non parte.

**BCDToolWindows risolve questo problema in modo diverso:**

- Le voci di avvio sono scritte direttamente nel firmware UEFI della scheda madre
- Se stacchi il disco Linux, il firmware salta automaticamente quella voce e avvia Windows
- Nessun bootloader intermedio, nessuna dipendenza tra i due sistemi
- Funziona come i firmware di fascia alta: ogni OS è completamente indipendente

---

## Funzionalità

| Funzionalità | Descrizione |
|---|---|
| **Scansione automatica** | Rileva tutti i sistemi operativi nelle partizioni ESP |
| **Aggiunta voci** | Aggiunge voci EFI (`BootXXXX`) direttamente nel firmware |
| **Rimozione voci** | Rimuove voci non più necessarie |
| **Ordinamento** | Cambia la priorità di avvio (`BootOrder`) |
| **Auto-configurazione** | Scansiona tutto e configura automaticamente |
| **Backup completo** | Salva l'intero stato UEFI in un file `.bcdtool` |
| **Ripristino** | Riporta il firmware esattamente com'era prima |
| **BootNext** | Avvia una volta un OS diverso senza cambiare l'ordine |
| **Abilita/Disabilita** | Attiva o disattiva singole voci senza cancellarle |

### Sistemi operativi rilevati automaticamente

- **Windows**: Boot Manager Microsoft
- **Debian/Ubuntu**: Ubuntu, Debian, Linux Mint, Pop!\_OS, elementary OS
- **Fedora/RHEL**: Fedora, CentOS, RHEL, AlmaLinux, Rocky Linux
- **SUSE**: openSUSE, SLES
- **Arch**: Arch Linux, EndeavourOS, Manjaro, Garuda
- **Altri**: Gentoo, NixOS, Void Linux, Kali Linux
- **Boot manager**: systemd-boot, rEFInd, OpenCore, Clover
- **BSD**: FreeBSD, OpenBSD, NetBSD
- **Fallback**: qualsiasi `\EFI\BOOT\BOOTx64.EFI`

---

## Requisiti di sistema

- Windows 10 / Windows 11 (o Windows 8 / Server 2012 come minimo)
- Firmware **UEFI** (non Legacy BIOS)
- Disco con tabella delle partizioni **GPT**
- Esecuzione come **Amministratore**

> Il programma verifica automaticamente questi requisiti all'avvio.

---

## Installazione e compilazione

### Opzione 1: Visual Studio (consigliata)

1. Installa [Visual Studio Community](https://visualstudio.microsoft.com/it/downloads/) con il workload **"Sviluppo desktop con C++"**
2. Installa [CMake](https://cmake.org/download/)
3. Clona il repository:
   ```cmd
   git clone https://github.com/tuonome/BCDToolWindows.git
   cd BCDToolWindows
   ```
4. Esegui `build.bat` oppure:
   ```cmd
   cmake -B build -G "Visual Studio 17 2022" -A x64 .
   cmake --build build --config Release
   ```
5. L'eseguibile si trova in `build\Release\BCDToolWindows.exe`

### Opzione 2: MinGW-w64 (MSYS2)

1. Installa [MSYS2](https://www.msys2.org/)
2. Dal terminale MSYS2 MinGW64:
   ```bash
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
   ```
3. Esegui `build.bat` dalla finestra di comando di Windows

### Opzione 3: Script diretto

```cmd
build.bat
```
Lo script rileva automaticamente il compilatore disponibile.

---

## Utilizzo

> **Esegui sempre come Amministratore** (clic destro → "Esegui come amministratore")

```
  ╔══════════════════════════════════════════════════════════╗
  ║          BCDToolWindows – EFI Boot Manager               ║
  ╚══════════════════════════════════════════════════════════╝

  MENU PRINCIPALE
  ──────────────────────────────────────────────────────────────

  1. Visualizza voci di avvio correnti
  2. Scansiona dischi e rileva sistemi operativi
  3. Aggiungi voce di avvio
  4. Rimuovi voce di avvio
  5. Cambia ordine di avvio
  6. Auto-configura tutto (consigliato)
  7. Backup e ripristino
  8. Impostazioni avanzate
  0. Esci
```

### Procedura consigliata per dual boot

1. Installa Linux su un secondo disco (durante l'installazione, **non installare GRUB** nel MBR, oppure lascialo solo sulla sua partizione EFI)
2. Avvia Windows
3. Esegui BCDToolWindows come Amministratore
4. Scegli **"6. Auto-configura tutto"**
5. Il programma crea automaticamente il backup e aggiunge le voci necessarie
6. Riavvia: vedrai il menu del firmware UEFI (o quello del programma) con entrambi i sistemi

### Ripristino completo

Se vuoi rimuovere completamente Linux e tornare all'avvio di sola Windows:

1. Stacca (o formatta) il disco Linux
2. Avvia BCDToolWindows
3. Scegli **"7. Backup e ripristino" → "Ripristina da backup"**
4. Seleziona il backup creato prima delle modifiche
5. Il firmware torna esattamente com'era

**Oppure, senza backup:**

1. Scegli **"4. Rimuovi voce di avvio"**
2. Rimuovi le voci relative a Linux
3. Windows rimane come voce principale

---

## Come funziona tecnicamente

### Variabili UEFI

Il firmware UEFI archivia le voci di avvio in variabili non volatili (NVRAM):

| Variabile | Contenuto |
|---|---|
| `BootOrder` | Lista ordinata di numeri `uint16_t` (es. `0001 0002 0000`) |
| `Boot0001` | Una voce di avvio (`EFI_LOAD_OPTION`) |
| `BootCurrent` | Voce usata per l'avvio corrente (sola lettura) |
| `BootNext` | Override per il prossimo avvio singolo |

### Struttura `EFI_LOAD_OPTION`

```
uint32_t  Attributes          // LOAD_OPTION_ACTIVE = 0x01
uint16_t  FilePathListLength  // Byte totali del device path
wchar_t   Description[]       // Nome (UTF-16, terminato da null)
byte[]    DevicePath          // Percorso del bootloader
byte[]    OptionalData        // Dati extra (ignorati di solito)
```

### Device path per un OS su disco GPT

```
HD(partNum, GPT, {GUID}, startLBA, sizeLBA)  →  42 byte
File(\EFI\ubuntu\grubx64.efi)               →  4 + pathLen byte
EndEntirePath                                →  4 byte
```

Il **GUID** nel `HD()` è il `UniquePartitionGUID` della partizione EFI (ESP), letto dalla tabella GPT.  
Se il disco viene rimosso, il firmware non trova la partizione con quel GUID e salta la voce automaticamente.

### API Windows usate

- `GetFirmwareEnvironmentVariableW` / `SetFirmwareEnvironmentVariableW` — lettura/scrittura variabili UEFI
- `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` — lettura tabella GPT (GUID partizioni)
- `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` — abbinamento volume ↔ partizione fisica
- `AdjustTokenPrivileges` — acquisizione di `SE_SYSTEM_ENVIRONMENT_PRIVILEGE`
- `SetVolumeMountPointW` / `DeleteVolumeMountPointW` — montaggio/smontaggio ESP

---

## Struttura del progetto

```
BCDToolWindows/
├── CMakeLists.txt          Build system
├── build.bat               Script di compilazione rapida
├── README.md               Questa documentazione
└── src/
    ├── efi_types.h         Strutture EFI/GPT (EFI_GUID, HARDDRIVE_DEVICE_PATH, ecc.)
    ├── privilege.h/.cpp    Gestione privilegi Windows (UAC, SE_SYSTEM_ENVIRONMENT)
    ├── gpt_reader.h/.cpp   Lettura dischi fisici e tabelle GPT
    ├── uefi_vars.h/.cpp    Lettura/scrittura variabili UEFI (BootXXXX, BootOrder)
    ├── esp_scanner.h/.cpp  Ricerca e montaggio partizioni ESP
    ├── os_detector.h/.cpp  Rilevamento sistemi operativi dalle ESP
    ├── boot_editor.h/.cpp  Operazioni ad alto livello sulle voci di avvio
    ├── backup.h/.cpp       Backup e ripristino dello stato UEFI
    └── main.cpp            Interfaccia utente (menu console)
```

---

## File di backup `.bcdtool`

Il backup viene salvato nella stessa cartella dell'eseguibile con il nome:
```
BCDToolWindows_snapshot_AAAA-MM-GG_HH-MM-SS.bcdtool
```

Il file è in formato binario e contiene:
- Tutti i valori delle variabili `BootXXXX` (blob binari completi)
- Il contenuto di `BootOrder`
- Data/ora e nota testuale

Per ripristinare: menu **7 → 2**, seleziona il file.

---

## Avvertenze e sicurezza

> ⚠️ **Modificare le variabili UEFI è un'operazione a basso livello.**  
> Un errore grave potrebbe rendere il sistema non avviabile.  
> **Crea sempre un backup prima di apportare modifiche.**

- Il programma crea un backup automatico prima di ogni modifica importante
- In caso di problemi: accedi al BIOS/UEFI e usa il menu di avvio nativo per ripristinare
- La funzione "Ripristina da backup" riporta tutto esattamente com'era
- In extremis: la maggior parte dei firmware UEFI ha un'opzione "Ripristina impostazioni predefinite" che ripristina le voci di avvio originali

---

## Differenze con programmi simili

| Caratteristica | BCDToolWindows | EasyBCD | rEFInd | GRUB |
|---|---|---|---|---|
| Modifica variabili UEFI native | ✅ | Parziale | ❌ | ❌ |
| Funziona senza Linux installato | ✅ | ✅ | ✅ | ❌ |
| Boot indipendente per disco | ✅ | ❌ | ❌ | ❌ |
| Rilevamento automatico OS | ✅ | ❌ | ✅ | ✅ |
| Backup/ripristino UEFI completo | ✅ | ❌ | ❌ | ❌ |
| Codice sorgente aperto | ✅ | ❌ | ✅ | ✅ |

---

## Licenza

MIT License — vedi [LICENSE](LICENSE) per i dettagli.

---

## Contribuire

Le pull request sono benvenute. Per modifiche importanti, apri prima una issue per discutere cosa vorresti cambiare.

Aree di miglioramento:
- Supporto per altri bootloader (aggiungere voci in `os_detector.cpp`)
- Interfaccia grafica (GUI con Win32 API o Qt)
- Identificazione versione OS da file di configurazione
- Supporto MBR (attualmente solo GPT)
