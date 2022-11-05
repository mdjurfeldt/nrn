# Test FUNCTION_TABLE with two dimensions (cagkftab.mod)
# Dimensions are voltage (mv) and ln(cai)
# Compare to cagk.mod

from neuron import h, gui
from math import log, exp, isclose

# from the cagk.mod file
abar = 0.48
bbar = 0.28
k1 = 0.18
k2 = 0.011
d1 = 0.84
d2 = 1.0
faraday = h.FARADAY / 1000.0  # killocoulombs


def alp(v, lnca):
    return abar / (1 + exp1(k1, d1, v, 0) / exp(lnca))


def bet(v, lnca):
    return bbar / (1 + exp(lnca) / exp1(k2, d2, v, 0))


def exp1(k, d, v, x):
    arg = -2 * d * faraday * v / h.R / (273.15 + h.celsius) + x
    if arg > 700.0:
        arg = 700.0
    if arg < -700.0:
        arg = -700.0
    return k * exp(arg)


# HOC matrix and 2-d double memory order is irow*ncol + jcol
def memorder():
    nrow = 3
    ncol = 4
    mat = h.Matrix(nrow, ncol)
    h("double a[%d][%d]" % (nrow, ncol))
    for i in range(nrow):
        for j in range(ncol):
            mat.x[i][j] = i * ncol + j
            h.a[i][j] = i * ncol + j
    rmat = mat._ref_x[0][0]
    ra = h._ref_a[0][0]
    for i in range(nrow * ncol):
        assert rmat[i] == float(i)
        assert ra[i] == float(i)


memorder()


def vecrange(min, max, step):
    return h.Vector().indgen(min, max, step)


def setup_tables():
    ln_ca_range = (-18, 4, 0.1)  # about 1e-9 to 1e2 in steps of 0.1 log unit
    v_range = (-80, 50, 1)  # -80mV to 50mV in 1mV steps
    casteps = vecrange(*ln_ca_range)
    vsteps = vecrange(*v_range)
    malp = h.Matrix(len(vsteps), len(casteps))
    mbet = malp.c()
    for i, v in enumerate(vsteps):
        for j, lnca in enumerate(casteps):
            malp.x[i][j] = alp(v, lnca)
            mbet.x[i][j] = bet(v, lnca)
            assert isclose(malp.x[i][j], h.alp_cagk(v, exp(lnca)), abs_tol=1e-9)
            assert isclose(mbet.x[i][j], h.bet_cagk(v, exp(lnca)), abs_tol=1e-9)

    # dimensions specified in size, min, max format
    h.table_alp_cagkftab(
        malp._ref_x[0][0],
        len(vsteps),
        vsteps[0],
        vsteps[len(vsteps) - 1],
        len(casteps),
        casteps[0],
        casteps[len(casteps) - 1],
    )
    # dimensions specified in size, pointer to array format (not efficient)
    h.table_bet_cagkftab(
        mbet._ref_x[0][0],
        len(vsteps),
        vsteps._ref_x[0],
        len(casteps),
        casteps._ref_x[0],
    )

    # must keep matrices and vectors in existence
    return malp, mbet, vsteps, casteps


tabs = setup_tables()


def cmp(v, lnca):
    print(v, lnca, h.alp_cagk(v, exp(lnca)), h.alp_cagkftab(v, lnca))


# at points where no interpolation is needed the table should be very good
def compare_exact():
    for lnca in tabs[3]:
        for v in tabs[2]:
            assert isclose(
                h.alp_cagk(v, exp(lnca)), h.alp_cagkftab(v, lnca), abs_tol=1e-9
            )
            assert isclose(
                h.bet_cagk(v, exp(lnca)), h.bet_cagkftab(v, lnca), abs_tol=1e-9
            )


compare_exact()


def compare():
    for cai in [1e-3, 1e-2, 1e-1]:
        for v in range(-80, 50):
            h.rate_cagk(v, cai)
            o = h.oinf_cagk
            h.rate_cagkftab(v, cai)
            oftab = h.oinf_cagkftab
            assert isclose(o, oftab, abs_tol=1e-4)


compare()


def ik_vs_t():
    g = h.Graph()
    g.size(0, 10, 0, 1)
    g.color(1)
    g.label(0.2, 0.95, "ik vs t (V = 20 mV)")
    g.label(0.8, 1, "")
    s = [h.Section(name="soma_" + str(i)) for i in range(2)]
    s[0].insert("cagk")
    s[1].insert("cagkftab")
    for sec in s:
        sec.diam = 10
        sec.L = 10
    seclamps = [h.SEClamp(i(0.5)) for i in s]
    for se in seclamps:
        se.dur1 = 10
        se.amp1 = 20
        se.dur2 = 1e9
        se.amp2 = -65
    tvec = h.Vector().record(h._ref_t, sec=s[0])
    ikvecs = [h.Vector().record(sec(0.5)._ref_ik, sec=sec) for sec in s]

    def plt(i):
        for j, line in enumerate(ikvecs):
            line.line(g, tvec, [8, i][j], [4, 1][j])
        assert ikvecs[0].c().sub(ikvecs[1]).abs().max() < 1e-4

    def runvc(i, cai):
        for sec in s:
            sec.cai = cai
        h.finitialize(-65)
        while h.t < 15.0:
            h.fadvance()
        plt(i)

    for i, cai in enumerate([1e-3, 1e-2, 1e-1]):
        g.color(i + 1)
        g.label("cai=%g" % cai)
        runvc(i + 1, cai)

    return g, tvec, ikvecs


p = ik_vs_t()
