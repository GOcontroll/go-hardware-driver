# go-hardware-driver

Generieke driver voor GOcontroll plug-in modules op Moduline-controllers (M1 / L4 / HMI1).

Leest `/lib/firmware/gocontroll/modules.json` (schema v1.0), instantieert per slot
de juiste module-driver, en publiceert/leest waarden via `/dev/shm/gocontroll/`.

## SHM layout

```
/dev/shm/gocontroll/
├── slot{N}/
│   ├── channel{C}/
│   │   ├── value          # input: gemeten waarde (driver schrijft)
│   │   │                  # output: commando (host schrijft)
│   │   ├── reset_value    # input only: pulse-counter reset value (host writes)
│   │   ├── reset_trigger  # input only: edge-triggered reset (host writes)
│   │   ├── current        # output feedback (mA, driver schrijft)
│   │   └── duty           # output feedback (driver schrijft)
│   ├── temperature       # module-level (output modules)
│   ├── ground
│   ├── supply
│   └── error_code        # output-10ch also has total_current
└── <name> -> slot{N}/channel{C}/value   (alias indien channel.name gegeven)
```

Alle waarden zijn ASCII-text (`%d\n`). Atomic single-writes met `pwrite(fd, buf, n, 0)`.

## Install via apt

```sh
sudo apt update
sudo apt install go-hardware-driver
```

De `postinst`-hook enable't en start de service automatisch.

## Build from source

Cross-compileert voor aarch64 met de GOcontroll-CodeBase als vendor-source
(zie [`GOcontroll/GOcontroll-CodeBase`](https://github.com/GOcontroll/GOcontroll-CodeBase)).

```sh
make GO_BASE=/path/to/GOcontroll-CodeBase
```

Output: `build/go-hardware-driver`. Manuele install:

```sh
sudo install -m 755 build/go-hardware-driver /usr/bin/
sudo install -m 644 debian/go-hardware-driver.service /usr/lib/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now go-hardware-driver
```

## Release naar apt.gocontroll.com

Een GitHub Actions workflow (`.github/workflows/build-package.yml`) bouwt
automatisch een `.deb` bij elke push van een `v*`-tag, plaatst die in een
GitHub Release, en triggert de index-rebuild op `apt.gocontroll.com`.

Eenmalig instellen vóór de eerste tag:
- **GitHub → Settings → Secrets and variables → Actions → New secret**:
  `APT_DISPATCH_TOKEN` = fine-grained PAT met Contents-write op
  `GOcontroll/go-apt`.

Daarna:

```sh
git tag v0.1.0
git push origin v0.1.0
```

## Tick

Vaste 100 Hz hoofdlus voor alle slots (`clock_nanosleep` `TIMER_ABSTIME`).
PWM-frequenties van output-modules worden uit `module.frequency_pairs` gelezen
en zijn iets anders dan de driver-tick (zie `output-6ch.md`).

## Pulse-counter / encoder reset

Voor input-modules wordt per kanaal een `reset_value` + `reset_trigger` paar in
shm gepubliceerd waarmee een host-app de pulse-counter (of encoder, twee
kanalen samen) kan resetten naar een gekozen waarde:

1. Host schrijft de gewenste counter-waarde naar `slot{N}/channel{C}/reset_value`
   (int32, ASCII).
2. Host **wijzigt** vervolgens `slot{N}/channel{C}/reset_trigger` naar een nieuwe
   waarde (uint8, elke wijziging = één reset-puls — bijv. opwaarts tellen).
3. Driver leest beide elke tick en delegeert naar
   `GO_module_input_reset_puls_counter()`. De CodeBase houdt de laatst-gezien
   trigger-byte bij in `pulscounterResetTrigger[c]` en stuurt **alleen** een
   SPI-reset als de byte gewijzigd is — edge-triggered semantiek, host hoeft
   geen self-clear te doen.

Bij driver-restart worden beide bestanden ge-truncate (anders zou een stale
trigger uit de vorige run, vergeleken met de verse `pulscounterResetTrigger=0`,
een spurious reset veroorzaken). Host moet na restart opnieuw schrijven om een
reset te requesten.

## Real-time tuning

De systemd-unit start de driver tijdens `sysinit.target` (zo vroeg mogelijk in de
boot, voor `multi-user.target`) met:

- `DefaultDependencies=no` — bypass standaard ordering en hardening om startup-
  tijd te minimaliseren.
- `Nice=-20` — hoogste scheduler-prioriteit voor SCHED_OTHER.
- `CPUAffinity=3` — pin op CPU 3, dezelfde core als `go-simulink`. Op de
  Moduline M1 (4 cores) blijft CPU 0/1/2 voor algemene system-load; alle
  real-time loops worden zo geclusterd op één geïsoleerde core voor cache-
  locality en voorspelbare scheduling.

## Module-type ondersteuning

| Type | Status |
|---|---|
| `input-6ch` | full |
| `output-6ch` | full |
| `input-10ch` | stub |
| `input-4-20ma` | stub |
| `output-10ch` | stub |
| `bridge-2ch` | stub |
