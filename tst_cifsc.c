// tst_cifsc.c - test gammatone-filterbank & instantaneous-compression
//              with WAV file input & ARSC output 

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

#include <arsclib.h>
#include <sigpro.h>
#include "chapro.h"
#define DATA_HDR "tst_cifio_data.h"
//#include DATA_HDR

#define MAX_MSG 256

typedef struct {
    char *ifn, *ofn, *dfn, mat, nrep;
    double rate;
    float *iwav, *owav;
    int32_t cs;
    int32_t *siz;
    int32_t iod, nwav, nsmp, mseg, nseg, oseg, pseg;
    void **out;
} I_O;

/***********************************************************/

static char   msg[MAX_MSG] = {0};
static double srate = 24000;   // sampling rate (Hz)
static int    chunk = 32;      // chunk size
static int    prepared = 0;
static int    io_wait = 40;
static struct {
    char *ifn, *ofn, simfb, afc, mat, nrep, play;
    double gn;
    int ds;
} args;
static CHA_CLS cls = {0};
static CHA_ICMP icmp = {0};

/***********************************************************/

static void
process_chunk(CHA_PTR cp, float *x, float *y, int cs)
{
    if (prepared) {
        // next line switches to compiled data
        //cp = (CHA_PTR) cha_data; 
        float *z = CHA_CB;
        // process filterbank+compressor
        cha_ciirfb_analyze(cp, x, z, cs);
        cha_icmp_process(cp, z, z, cs);
        cha_ciirfb_synthesize(cp, z, y, cs);
    }
}

/***********************************************************/

// initialize io

static void
usage()
{
    printf("usage: tst_cifsc [-options] [input_file] [output_file]\n");
    printf("options\n");
    printf("-c N  compress with gain=N (dB) [0]\n");
    printf("-d N  set downsample factor to N [24]\n");
    printf("-h    print help\n");
    printf("-k N  compression kneepoint=N (dB) [0]\n");
    printf("-m    output MAT file\n");
    printf("-p    play output\n");
    printf("-v    print version\n");
    exit(0);
}

static void
version()
{
    printf("%s\n", cha_version());
    exit(0);
}

static int
mat_file(char *fn)
{
    int d;

    if (fn) {
        d = strlen(fn) - 4;
        if (d > 0) {
            if ((tolower(fn[d + 1]) == 'm')
             && (tolower(fn[d + 2]) == 'a')
             && (tolower(fn[d + 3]) == 't')) {
                return (1);
            }
        }
    }

    return (0);
}

static void
parse_args(int ac, char *av[])
{
    args.ifn = "test/cat.wav";
    args.mat = 1;
    args.play = 0;
    args.ds = 0;
    args.gn = 0;
    while (ac > 1) {
        if (av[1][0] == '-') {
            if (av[1][1] == 'c') {
                args.gn = atof(av[2]);
                ac--;
                av++;
            } else if (av[1][1] == 'd') {
                args.ds = atoi(av[2]);
                ac--;
                av++;
            } else if (av[1][1] == 'h') {
                usage();
            } else if (av[1][1] == 'm') {
                args.mat = 1;
            } else if (av[1][1] == 'p') {
                args.play = 1;
            } else if (av[1][1] == 'v') {
                version();
            }
            ac--;
            av++;
        } else {
            break;
        }
    }
    //args.ifn = (ac > 1) ? av[1] : NULL;
    args.ofn = (ac > 2) ? av[2] : NULL;
    if (args.ofn) args.mat = mat_file(args.ofn);
}

/***********************************************************/

void
msleep(uint32_t msec)
{
#ifdef WIN32
    Sleep(msec);
#else
    struct timespec delay = {0};
    uint32_t sec = msec / 1000;
    msec -= sec * 1000;
    delay.tv_sec  = sec;
    delay.tv_nsec = msec * 1000000; // convert msec to nsec
    nanosleep(&delay, &delay);
#endif
}

/***********************************************************/

static void
set_spl(float *x, int n, double rms_lev, double spl_ref)
{
    float scl;
    double xx, rms, smsq, lev;
    int i;

    smsq = 0;
    for (i = 0; i < n; i++) {
        xx = x[i];
        smsq += xx * xx;
    }
    rms = sqrt(smsq / n);
    lev = 20 * log10(rms / spl_ref);
    scl = (float) pow(10,(rms_lev - lev) / 20);
    for (i = 0; i < n; i++) {
        x[i] *= scl;
    }
}

static int
init_wav(I_O *io, char *msg)
{
    float fs;
    VAR *vl;
    static double spl_ref = 1.1219e-6;
    static double rms_lev = 65;

    if (io->iwav) free(io->iwav);
    if (io->owav) free(io->owav);
    if (io->ifn) {
        // get WAV file info
        vl = sp_wav_read(io->ifn, 0, 0, &fs);
        if (vl == NULL) {
            fprintf(stderr, "can't open %s\n", io->ifn);
            return (1);
        }
        if (io->rate != fs) {
            fprintf(stderr, "WARNING: %s rate mismatch: ", io->ifn);
            fprintf(stderr, "%.0f != %.0f\n", fs, io->rate);
            io->rate = fs;
        }
        if (msg) sprintf(msg, " WAV input : %s repeat=%d\n", io->ifn, io->nrep);
        io->nwav = vl[0].rows * vl[0].cols;
        io->iwav = (float *) calloc(io->nwav, sizeof(float));
        fcopy(io->iwav, vl[0].data, io->nwav);
        set_spl(io->iwav, io->nwav, rms_lev, spl_ref);
        sp_var_clear(vl);
    } else {    /* ADC input */
        io->nwav = 0;
        io->iwav = (float *) calloc(io->cs * 2, sizeof(float));
    }
    if (io->ofn) {
        io->nsmp = io->nwav;
        io->mseg = 1;
        io->nseg = 1;
        io->owav = (float *) calloc(io->nsmp, sizeof(float));
    } else {    /* DAC output */
        io->cs = round((io->rate * io_wait * 4) / 1000); // chunk size
        io->mseg = 2;
        io->nseg = io->nrep * io->nwav  / io->cs;
        io->owav = (float *) calloc(io->cs * (io->mseg + 1), sizeof(float));
	io->nsmp = io->nwav * io->nrep;
    } 
    io->pseg = io->mseg;
    return (0);
}

/***********************************************************/

static void
init_aud(I_O *io)
{
#ifdef ARSCLIB_H
    char name[80];
    int i, j, err;
    static int nchn = 2;        // number of channels
    static int nswp = 0;        // number of sweeps (0=continuous)
    static int32_t fmt[2] = {ARSC_DATA_F4, 0};

    err = ar_out_open(io->iod, io->rate, nchn);
    if (err) {
        ar_err_msg(err, msg, MAX_MSG);
        fprintf(stderr, "ERROR: %s\n", msg);
        return;
    }
    ar_dev_name(io->iod, name, 80);
    ar_set_fmt(io->iod, fmt);
    io->siz = (int32_t *) calloc(io->mseg, sizeof(int32_t));
    io->out = (void **) calloc(io->mseg * nchn, sizeof(void *));
    for (i = 0; i < io->mseg; i++) {
        io->siz[i] = io->cs;
        io->out[i * nchn] = io->owav + io->cs * i;
        for (j = 1; j < nchn; j++) {
            io->out[i * nchn + j] = NULL;
        }
    }
    ar_out_prepare(io->iod, io->out, (int32_t *)io->siz, io->mseg, nswp);
    printf("audio output: %s\n", name);
    ar_io_start(io->iod);
#endif // ARSCLIB_H
}

static int
get_aud(I_O *io)
{
#ifdef ARSCLIB_H
    io->oseg = ar_io_cur_seg(io->iod);
#endif // ARSCLIB_H
    return (io->oseg < io->nseg);
}

static void
put_aud(I_O *io, CHA_PTR cp)
{
    int od, iw, ow, nd, ns;

    if ((io->oseg + io->mseg) == io->pseg) {
        od = io->pseg * io->cs;
        nd = io->nrep * io->nwav - od;
        ow = (io->pseg % io->mseg) * io->cs;
        iw = od % io->nwav;
        ns = (io->cs > (io->nwav - iw)) ? (io->nwav - iw) : io->cs;
        if (nd >= io->cs) {
            if (ns == io->cs) {
                fcopy(io->owav + ow, io->iwav + iw, io->cs);
            } else {
                fcopy(io->owav + ow, io->iwav + iw, ns);
                fcopy(io->owav + ow, io->iwav, io->cs - ns);
            }
        } else if (nd > 0) {
            if (ns == io->cs) {
                fcopy(io->owav + ow, io->iwav + iw, nd);
                fzero(io->owav + ow + nd, 2 * io->cs - nd);
            } else {
                fcopy(io->owav + ow, io->iwav + iw, nd);
                fcopy(io->owav + ow + nd, io->iwav + iw, ns - nd);
                fzero(io->owav + ow + ns, 2 * io->cs - ns);
            } 
        } else {
            fzero(io->owav, 2 * io->cs);
        }
        io->pseg++;
        process_chunk(cp, io->owav + ow, io->owav + ow, io->cs);
    }
}

/***********************************************************/

// terminate io

static void
write_wave(I_O *io)
{
    float r[1], *w;
    int   n, nbits = 16;
    static VAR *vl;

    if (io->ofn) {
        printf(" WAV output: %s\n", io->ofn);
        r[0] = (float) io->rate;
        n = io->nwav;
        w = io->owav;
        vl = sp_var_alloc(2);
        sp_var_add(vl, "rate",        r,       1, 1, "f4");
        sp_var_add(vl, "wave",        w,       n, 1, "f4");
        vl[1].dtyp = SP_DTYP_F4; /* workaround sigpro bug */
        remove(io->ofn);
        sp_wav_write(io->ofn, vl + 1, r, nbits);
        sp_var_clear(vl);
    }
}

static void
stop_wav(I_O *io)
{
    if (io->ofn) {
        free(io->owav);
    } else {
#ifdef ARSCLIB_H
        ar_io_stop(io->iod);
        ar_io_close(io->iod);
#endif // ARSCLIB_H
        if (io->siz) free(io->siz);
        if (io->out) free(io->out);
        if (io->owav) free(io->owav);
    }
    if (io->ifn) {
        sp_var_clear_all();
    } else {
        free(io->iwav);
    }
    if (io->nseg == 1) {
        printf("...done");
    }
    printf("\n");
}

/***********************************************************/

// specify filterbank center frequecies and bandwidths

static double
cgtfb_init(CHA_CLS *cls, double sr)
{
    float lfbw, fmid = 1000;
    int i, nh, nc, nm, po;

    nm = icmp.nm;
    po = icmp.po;
    lfbw = fmid / nm;
    nh = (int) floor(log2((float)sr / 2000) * po);
    nc = nh + nm;
    cls->nc = nc;
    for (i = 0; i < (nm - 1); i++) {
        cls->fc[i] = lfbw * (i + 1);
        cls->bw[i] = lfbw;
    }
    cls->fc[nm - 1] = fmid;
    cls->bw[nm - 1] = fmid * (pow(2.0, 0.5 / po) - (nm - 0.5) / nm);
    for (i = nm; i < nc; i++) {
        cls->fc[i] = fmid * pow(2.0, (i - nm + 1.0) / po);
        cls->bw[i] = fmid * (pow(2.0, (i - nm + 1.5) / po)
                           - pow(2.0, (i - nm + 0.5) / po));
    }

    return (400 / lfbw);
}

// CSL prescription

static void
compressor_init(CHA_CLS *cls)
{
    double gn;
    int k, nc;

    gn = icmp.gn;
    // set compression mode
    cls->cm = 1;
    // loop over filterbank channel
    nc = cls->nc;
    for (k = 0; k < nc; k++) {
        cls->Lcs[k] = 0;
        cls->Lcm[k] = 50;
        cls->Lce[k] = 100;
        cls->Lmx[k] = 120;
        cls->Gcs[k] = (float) gn;
        cls->Gcm[k] = (float) gn / 2;
        cls->Gce[k] = 0;
        cls->Gmx[k] = 90;
    }
}

/***********************************************************/

// prepare filterbank

static void
prepare_filterbank(CHA_PTR cp)
{
    double gd, sr, *fc, *bw;
    float z[256], p[256], g[64]; 
    int cs, nc, no, d[32];

    sr = srate;
    cs = chunk;
    cgtfb_init(&cls, sr);
    // prepare filterbank
    no = icmp.no;      // gammatone filter order
    gd = icmp.gd;      // target delay (ms) 
    nc = cls.nc;
    fc = cls.fc;
    bw = cls.bw;
    cha_ciirfb_design(z, p, g, d, nc, fc, bw, sr, gd);
    cha_ciirfb_prepare(cp, z, p, g, d, nc, no, sr, cs);
    printf(" prepare_fb: nc=%d gd=%.1f\n", nc, gd);
}

// prepare compressor

static void
prepare_compressor(CHA_PTR cp, double sr)
{
    static double lr = 2e-5;    // signal-level reference (Pa)
    static int    ds = 24;      // downsample factor

    compressor_init(&cls);
    if (args.ds) ds = args.ds;
    cha_icmp_prepare(cp, &cls, sr, lr, ds);
}

// prepare input/output

static int
prepare_io(I_O *io)
{
    // initialize waveform
    io->rate = srate;
    io->cs   = chunk;
    if (init_wav(io, msg)) {
        return (1);
    }
    // prepare i/o
    if (!io->ofn) {
        init_aud(io);
    }
    printf("%s", msg);
    printf(" prepare_io: sr=%.0f cs=%d ns=%d\n", io->rate, io->cs, io->nsmp);
    return (0);
}

// prepare signal processing

static void
prepare(I_O *io, CHA_PTR cp, double sr, int cs)
{
    prepare_io(io);
    srate = io->rate;
    chunk = io->cs;
    prepare_filterbank(cp);
    prepare_compressor(cp, sr);
    // generate C code from prepared data
    //cha_data_gen(cp, DATA_HDR);
    prepared++;
}

// process signal

static void
process(I_O *io, CHA_PTR cp)
{
    float *x, *y;
    int i, n, cs, nk;
    double t1, t2;

    if (io->ofn) {
        sp_tic();
        // initialize i/o pointers
        x = io->iwav;
        y = io->owav;
        n = io->nwav;
        cs = io->cs;        // chunk size
        nk = n / cs;        // number of chunks
        for (i = 0; i < nk; i++) {
            process_chunk(cp, x + i * cs, y + i * cs, cs);
        }
        t1 = sp_toc();
        t2 = io->nwav / io->rate;
        printf("speed_ratio: ");
        printf("(wave_time/wall_time) = (%.3f/%.3f) ", t2, t1);
        printf("= %.1f\n", t2 / t1);
    } else {
        while (get_aud(io)) {
            put_aud(io, cp);
            msleep(io_wait); // wait time
        }
    }
}

// clean up io

static void
cleanup(I_O *io, CHA_PTR cp)
{
    if (io->ofn) {
        write_wave(io);
    }
    stop_wav(io);
    cha_cleanup(cp);
}

/***********************************************************/

static void
configure_compressor()
{
    // Example of instantaneous compression with IIR filterbank
    icmp.gn = 20;      // flat suppressor gain (dB)
    icmp.gd =  4;      // target_delay (ms)
    icmp.nm =  5;      // number of frequency bands below 1 kHz
    icmp.po =  3;      // number of bands per octave above 1 kHz
    icmp.no =  4;      // gammatone filter order 
    if (args.gn) icmp.gn = args.gn;
}

static void
configure(I_O *io)
{
    static char *ifn = "test/carrots.wav";
    static char *wfn = "test/tst_cifsc.wav";
    static char *mfn = "test/tst_cifsc.mat";

    // initialize CHAPRO variables
    configure_compressor();
    // initialize I/O
#ifdef ARSCLIB_H
    io->iod = ar_find_dev(ARSC_PREF_SYNC); // find preferred audio device
#endif // ARSCLIB_H
    io->iwav = NULL;
    io->owav = NULL;
    io->ifn  = args.ifn  ? args.ifn : ifn;
    io->ofn  = args.play ? args.ofn : wfn; 
    io->dfn  = mfn; 
    io->mat  = args.mat;
    io->nrep = (args.nrep < 1) ? 1 : args.nrep;
}

static void
report(double sr)
{
    // report
    printf("CHA simulation: sampling rate=%.0f Hz, ", sr);
    printf("filterbank gd=%.1f ms; ", icmp.gd);
    printf("IIR + inst. compression\n");
}

/***********************************************************/

int
main(int ac, char *av[])
{
    static double sr = 24000;
    static int    cs = 32;
    static void *cp[NPTR] = {0};
    static I_O io;

    parse_args(ac, av);
    configure(&io);
    report(sr);
    prepare(&io, cp, sr, cs);
    process(&io, cp);
    cleanup(&io, cp);
    return (0);
}
