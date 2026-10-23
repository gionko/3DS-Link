#include "pace.h"

#include <math.h>
#include <string.h>

// Passo nominale storico: 1,85 MiB/s di dati / 1456 byte.
#define NOMINAL_RATE 1332.0

static const double kFixed[TS_PACE_MODES] = {0, NOMINAL_RATE, 1500.0, 1650.0, 1800.0, 2000.0, TS_RAMP_START, TS_FREE_RATE};

const char* pace_mode_name(int mode) {
    static const char* names[TS_PACE_MODES] = {"adattivo", "fisso 1332/s", "fisso 1500/s", "fisso 1650/s", "fisso 1800/s", "fisso 2000/s", "rampa 1100-2600/s", "libero (senza passo)"};
    return (mode >= 0 && mode < TS_PACE_MODES) ? names[mode] : "?";
}

static double clamp(const ts_pace* p, double r) {
    return r < p->minRate ? p->minRate : (r > p->maxRate ? p->maxRate : r);
}

void pace_init(ts_pace* p, int mode) {
    memset(p, 0, sizeof(*p));
    if(mode < 0 || mode >= TS_PACE_MODES) {
        mode = 1;
    }

    p->mode = mode;
    p->minRate = 450.0;
    p->maxRate = 2200.0;
    p->rate = mode == 0 ? NOMINAL_RATE : kFixed[mode];
    p->baseRate = p->rate;
}

void pace_update(ts_pace* p, double sent, double req, double dt) {
    if(dt <= 0) {
        return;
    }

    double actual = sent / dt;
    double loss = sent > 20.0 ? req / sent : 0.0;
    if(loss > 1.0) {
        loss = 1.0;
    }

    double good = actual * (1.0 - loss);
    p->lossEma = p->periods == 0 ? loss : 0.5 * p->lossEma + 0.5 * loss;
    p->goodEma = p->periods == 0 ? good : 0.5 * p->goodEma + 0.5 * good;
    p->refGood = good > p->refGood * 0.97 ? good : p->refGood * 0.97;
    p->periods++;
    p->elapsed += dt;
    if(p->mode == 6) {
        double r = TS_RAMP_START + TS_RAMP_STEP * (double) (int) (p->elapsed / TS_RAMP_STEP_SEC);
        p->rate = r > TS_RAMP_END ? TS_RAMP_END : r;
        return;
    }

    if(p->mode != 0) {
        return;
    }

    // quasi niente inviato (finestra piena, nessun ricevente): non c'e' nulla da misurare
    if(sent < 20.0 * dt) {
        return;
    }

    // Il passo impostato e' sopra quello che la radio regge: si riporta sulla capacita' vista, senza prove inutili.
    if(actual < 0.85 * p->rate) {
        p->rate = clamp(p, actual * 1.08);
        p->phase = 0;
        p->hold = 1;
        p->baseRate = p->rate;
        p->baseGood = good;
        return;
    }

    if(p->hold > 0) {
        p->hold--;
        p->baseRate = p->rate;
        p->baseGood = good;
        p->phase = 0;
        return;
    }

    if(p->phase == 0) {
        p->baseRate = p->rate;
        p->baseGood = good;
        if(p->dir == 0) {
            p->dir = loss > 0.06 ? -1 : 1;
        }

        double t = clamp(p, p->rate * (1.0 + 0.08 * p->dir));
        if(fabs(t - p->rate) < 1.0) {
            p->dir = -p->dir; // contro un limite: si prova dall'altra parte la prossima volta
            p->hold = 3;
            return;
        }

        p->rate = t;
        p->phase = 1;
        return;
    }

    // phase 1: il passo di prova e' stato tenuto per un periodo intero
    if(good > p->baseGood * 1.03) {
        // meglio: lo si tiene e si prosegue nella stessa direzione
        p->baseRate = p->rate;
        p->baseGood = good;
        double t = clamp(p, p->rate * (1.0 + 0.08 * p->dir));
        if(fabs(t - p->rate) < 1.0) {
            p->phase = 0;
            p->hold = 4;
        } else {
            p->rate = t;
        }

        return;
    }

    if(p->dir < 0 && good >= p->refGood * 0.985) {
        // praticamente uguale con meno aria usata: si tiene il passo piu' basso. La base resta quella di partenza,
        // cosi' piccole perdite successive non si sommano.
        p->baseRate = p->rate;
        p->phase = 0;
        p->hold = 2;
        return;
    }

    // peggio (o uguale salendo): si torna indietro e si prova dall'altra parte piu' tardi
    p->rate = p->baseRate;
    p->dir = -p->dir;
    p->phase = 0;
    p->hold = p->dir > 0 ? 3 : 6;
}
