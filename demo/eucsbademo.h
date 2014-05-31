#ifndef _SBA_DEMO_H_
#define _SBA_DEMO_H_

#define SBA_MAX_REPROJ_ERROR    4.0 // max motion only reprojection error

#define BA_NONE                 -1
#define BA_MOTSTRUCT            0
#define BA_MOT                  1
#define BA_STRUCT               2
#define BA_MOT_MOTSTRUCT        3

/* in imgproj.c */
extern void calcImgProj(double a[5], double qr0[4], double v[3], double t[3], double M[3], double n[2]);
extern void calcImgProjFullR(double a[5], double qr0[4], double t[3], double M[3], double n[2]);
extern void calcImgProjJacKRTS(double a[5], double qr0[4], double v[3], double t[3], double M[3], double jacmKRT[2][11], double jacmS[2][3]);
extern void calcImgProjJacKRT(double a[5], double qr0[4], double v[3], double t[3], double M[3], double jacmKRT[2][11]);
extern void calcImgProjJacS(double a[5], double qr0[4], double v[3], double t[3], double M[3], double jacmS[2][3]);
extern void calcImgProjJacRTS(double a[5], double qr0[4], double v[3], double t[3], double M[3], double jacmRT[2][6], double jacmS[2][3]);
extern void calcImgProjJacRT(double a[5], double qr0[4], double v[3], double t[3], double M[3], double jacmRT[2][6]);
extern void calcDistImgProj(double a[5], double kc[5], double qr0[4], double v[3], double t[3], double M[3], double n[2]);
extern void calcDistImgProjFullR(double a[5], double kc[5], double qr0[4], double t[3], double M[3], double n[2]);
extern void calcDistImgProjJacKDRTS(double a[5], double kc[5], double qr0[4], double v[3], double t[3], double M[3], double jacmKDRT[2][16], double jacmS[2][3]);
extern void calcDistImgProjJacKDRT(double a[5], double kc[5], double qr0[4], double v[3], double t[3], double M[3], double jacmKDRT[2][16]);
extern void calcDistImgProjJacS(double a[5], double kc[5], double qr0[4], double v[3], double t[3], double M[3], double jacmS[2][3]);

/* in eucsbademo.c */
extern void sba_driver(char *camsfname, char *ptsfname, char *calibfname, int cnp, int pnp, int mnp,
                       void (*caminfilter)(double *pin, int nin, double *pout, int nout),
                       void (*camoutfilter)(double *pin, int nin, double *pout, int nout),
                       int cnfp, char *refcamsfname, char *refptsfname);

#endif /* _SBA_DEMO_H_ */
