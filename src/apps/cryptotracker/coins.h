#pragma once
#include <stdint.h>

// Paire de cotation Binance (USDT ~ USD). Les grandes cryptos existent aussi en EUR
// (BTCEUR, ETHEUR...) : changer QUOTE et CURRENCY_SUFFIX si besoin.
#define QUOTE "USDT"
#define CURRENCY_SUFFIX " $"

struct Coin {
  const char *sym;    // symbole Binance (sans la quote)
  const char *name;   // nom affiché
  uint32_t color;     // couleur du badge
};

struct Timeframe {
  const char *label;     // libellé du bouton
  const char *interval;  // intervalle des bougies Binance
  int limit;             // nombre de bougies
};

static const Coin COINS[] = {
  {"BTC",  "Bitcoin",   0xF7931A},
  {"ETH",  "Ethereum",  0x627EEA},
  {"SOL",  "Solana",    0x14F195},
  {"BNB",  "BNB",       0xF3BA2F},
  {"XRP",  "XRP",       0x00AAE4},
  {"ADA",  "Cardano",   0x3468D1},
  {"DOGE", "Dogecoin",  0xC2A633},
  {"AVAX", "Avalanche", 0xE84142},
  {"DOT",  "Polkadot",  0xE6007A},
  {"LINK", "Chainlink", 0x2A5ADA},
  {"LTC",  "Litecoin",  0xBFBBBB},
  {"TRX",  "TRON",      0xFF3B3B},
};
static const int NUM_COINS = sizeof(COINS) / sizeof(COINS[0]);

static const Timeframe TFS[] = {
  {"1H",  "1m",  60},
  {"24H", "15m", 96},
  {"7J",  "2h",  84},
  {"30J", "8h",  90},
};
static const int NUM_TFS = sizeof(TFS) / sizeof(TFS[0]);
#define MAX_POINTS 96
