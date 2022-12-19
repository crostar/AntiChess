## Antichess
This project builds an AI for Antichess (not really intelligent) based on a random playing strategy. It is based on the open-source project Stockfish, **The main game logic is in file** `uci.cpp`.

## Build
```{bash}
cd src
make net
make build ARCH=x86-64-modern
```

## Run
```{bash}
./stockfish [white | black]
```
