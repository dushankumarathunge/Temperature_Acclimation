#include "water_balance.h"
#include "zbrent.h"
#include "nrutil.h"
#include "odeint.h"

void initialise_soils(control *c, fluxes *f, params *p, state *s) {
    /* Initialise soil water state & parameters  */

    double *fsoil_top = NULL, *fsoil_root = NULL;
    int     i;

    /* site params not known, so derive them based on Cosby et al */

    if (c->calc_sw_params) {
        fsoil_top = get_soil_fracs(p->topsoil_type);
        fsoil_root = get_soil_fracs(p->rootsoil_type);

        /* top soil */
        calc_soil_params(fsoil_top, &p->theta_fc_topsoil, &p->theta_wp_topsoil,
                         &p->theta_sp_topsoil, &p->b_topsoil,
                         &p->psi_sat_topsoil);

        /* Plant available water in top soil (mm) */
        p->wcapac_topsoil = p->topsoil_depth  *\
                            (p->theta_fc_topsoil - p->theta_wp_topsoil);
        /* Root zone */
        calc_soil_params(fsoil_root, &p->theta_fc_root, &p->theta_wp_root,
                         &p->theta_sp_root, &p->b_root, &p->psi_sat_root);

        /* Plant available water in rooting zone (mm) */
        p->wcapac_root = p->rooting_depth * \
                            (p->theta_fc_root - p->theta_wp_root);
    }


    /* calculate Landsberg and Waring SW modifier parameters if not
       specified by the user based on a site calibration */
    if (p->ctheta_topsoil < -900.0 && p->ntheta_topsoil  < -900.0 &&
        p->ctheta_root < -900.0 && p->ntheta_root < -900.0) {
        get_soil_params(p->topsoil_type, &p->ctheta_topsoil, &p->ntheta_topsoil);
        get_soil_params(p->rootsoil_type, &p->ctheta_root, &p->ntheta_root);
    }
    /*
    printf("%f\n", p->wcapac_topsoil);
    printf("%f\n\n", p->wcapac_root);

    printf("%f\n", p->ctheta_topsoil);
    printf("%f\n", p->ntheta_topsoil);
    printf("%f\n", p->ctheta_root);
    printf("%f\n", p->ntheta_root);
    printf("%f\n", p->rooting_depth);

    exit(1); */

    /* Set up all the hydraulics stuff */
    if (c->water_balance == HYDRAULICS) {
        calc_saxton_stuff(p, fsoil_root);

        f->soil_conduct = malloc(p->n_layers * sizeof(double));
        if (f->soil_conduct == NULL) {
            fprintf(stderr, "malloc failed allocating soil_conduct\n");
            exit(EXIT_FAILURE);
        }

        f->swp = malloc(p->n_layers * sizeof(double));
        if (f->swp == NULL) {
            fprintf(stderr, "malloc failed allocating swp\n");
            exit(EXIT_FAILURE);
        }

        f->soilR = malloc(p->n_layers * sizeof(double));
        if (f->soilR == NULL) {
            fprintf(stderr, "malloc failed allocating soilR\n");
            exit(EXIT_FAILURE);
        }

        f->fraction_uptake = malloc(p->n_layers * sizeof(double));
        if (f->fraction_uptake == NULL) {
            fprintf(stderr, "malloc failed allocating soilR\n");
            exit(EXIT_FAILURE);
        }

        f->ppt_gain = malloc(p->n_layers * sizeof(double));
        if (f->ppt_gain == NULL) {
            fprintf(stderr, "malloc failed allocating ppt_gain\n");
            exit(EXIT_FAILURE);
        }

        f->water_loss = malloc(p->n_layers * sizeof(double));
        if (f->water_loss == NULL) {
            fprintf(stderr, "malloc failed allocating water_loss\n");
            exit(EXIT_FAILURE);
        }

        f->water_gain = malloc(p->n_layers * sizeof(double));
        if (f->water_gain == NULL) {
            fprintf(stderr, "malloc failed allocating water_gain\n");
            exit(EXIT_FAILURE);
        }


        /* Depth to bottom of wet soil layers (m) */
        s->water_frac = malloc(p->n_layers * sizeof(double));
        if (s->water_frac == NULL) {
            fprintf(stderr, "malloc failed allocating water_frac\n");
            exit(EXIT_FAILURE);
        }

        /* Depth to bottom of wet soil layers (m) */
        s->wetting_bot = malloc(p->wetting * sizeof(double));
        if (s->wetting_bot == NULL) {
            fprintf(stderr, "malloc failed allocating wetting_bot\n");
            exit(EXIT_FAILURE);
        }

        /* Depth to top of wet soil layers (m) */
        s->wetting_top = malloc(p->wetting * sizeof(double));
        if (s->wetting_top == NULL) {
            fprintf(stderr, "malloc failed allocating wetting_top\n");
            exit(EXIT_FAILURE);
        }
        for (i = 0; i < p->wetting; i++) {
            s->wetting_bot[i] = 0.0;
            s->wetting_top[i] = 0.0;
        }

        /* saturate the top layer */
        s->wetting_bot[0] = s->thickness[0];

        /* Initalise SW fraction - we should read this from param file */
        s->initial_water = 0.0;
        for (i = 0; i < p->n_layers; i++) {
            s->water_frac[i] = 0.4;
            s->initial_water += 1E3 * (s->water_frac[i] * s->thickness[i]);
        }

    }

    free(fsoil_top);
    free(fsoil_root);

    return;
}


void calculate_water_balance(control *c, fluxes *f, met *m, params *p,
                             state *s, int daylen, double trans_leaf,
                             double omega_leaf, double rnet_leaf) {
    /*

    Calculate the water balance (including all water fluxes).

    Parameters:
    ----------
    control : structure
        control structure
    fluxes : structure
        fluxes structure
    met : structure
        meteorological drivers structure
    params : structure
        parameters structure
    day : int
        project day. (Dummy argument, only passed for daily model)
    daylen : double
        length of day in hours. (Dummy argument, only passed for daily model)
    trans_leaf : double
        leaf transpiration (Dummy argument, only passed for sub-daily model)
    omega_leaf : double
        decoupling coefficient (Dummy argument, only passed for sub-daily model)
    rnet_leaf : double
        total canopy rnet (Dummy argument, only passed for sub-daily model)

    */
    double soil_evap, et, interception, runoff, conv,
           transpiration, net_rad, SEC_2_DAY, DAY_2_SEC,
           transpiration_am, transpiration_pm, gs_am, gs_pm, LE_am,
           LE_pm, ga_am, ga_pm, net_rad_am, net_rad_pm, omega_am,
           gpp_am, gpp_pm, omega_pm, throughfall,
           canopy_evap;

    SEC_2_DAY = 60.0 * 60.0 * daylen;
    DAY_2_SEC = 1.0 / SEC_2_DAY;

    if (c->water_balance == HYDRAULICS) {
        calculate_water_balance_hydraulics(c, f, m, p, s, daylen, trans_leaf,
                                           omega_leaf, rnet_leaf);

    }

    if (c->sub_daily) {
        /* calculate potential canopy evap rate, this may be reduced later
           depending on canopy water storage */
        canopy_evap = calc_canopy_evaporation(m, p, s, rnet_leaf);

        /* mol m-2 s-1 to mm/day */
        conv = MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_HLFHR;
        canopy_evap *= conv;
        calc_interception(c, m, p, f, s, &throughfall, &interception,
                          &canopy_evap);
    } else {
        /* don't need to work out the canopy evap */
        calc_interception(c, m, p, f, s, &throughfall, &interception,
                          &canopy_evap);

    }

    net_rad = calc_net_radiation(p, m->sw_rad, m->tair);
    soil_evap = calc_soil_evaporation(m, p, s, net_rad);
    if (c->sub_daily) {
        soil_evap *= MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_HLFHR;
    } else {
        net_rad_am = calc_net_radiation(p, m->sw_rad_am, m->tair_am);
        net_rad_pm = calc_net_radiation(p, m->sw_rad_pm, m->tair_pm);
        soil_evap *= MOLE_WATER_2_G_WATER * G_TO_KG * (60.0 * 60.0 * daylen);
    }

    if (c->sub_daily) {
        /* mol m-2 s-1 to mm/30 min */
        transpiration = trans_leaf * MOLE_WATER_2_G_WATER * G_TO_KG * \
                        SEC_2_HLFHR;

    } else {
        /* gC m-2 day-1 -> umol m-2 s-1 */
        conv = GRAMS_C_TO_MOL_C * MOL_TO_UMOL * DAY_2_SEC;
        gpp_am = f->gpp_am * conv;
        gpp_pm = f->gpp_pm * conv;

        penman_canopy_wrapper(p, s, m->press, m->vpd_am, m->tair_am, m->wind_am,
                              net_rad_am, m->Ca, gpp_am, &ga_am, &gs_am,
                              &transpiration_am, &LE_am, &omega_am);
        penman_canopy_wrapper(p, s, m->press, m->vpd_pm, m->tair_pm, m->wind_pm,
                              net_rad_pm, m->Ca, gpp_pm, &ga_pm, &gs_pm,
                              &transpiration_pm, &LE_pm, &omega_pm);

        /* mol m-2 s-1 to mm/day */
        conv = MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_DAY;
        transpiration = (transpiration_am + transpiration_pm) * conv;

        f->omega = (omega_am + omega_pm) / 2.0;

        /* output in mol H20 m-2 s-1 */
        f->gs_mol_m2_sec = gs_am + gs_pm;
        f->ga_mol_m2_sec = ga_am + ga_pm;
    }

    /*
    ** NB. et, transpiration & soil evap may all be adjusted in
    ** update_water_storage if we don't have sufficient water
    */
    et = transpiration + soil_evap + canopy_evap;

    update_water_storage(c, f, p, s, throughfall, interception, canopy_evap,
                         &transpiration, &soil_evap, &et, &runoff);

    if (c->sub_daily) {
        sum_hourly_water_fluxes(f, soil_evap, transpiration, et, interception,
                                throughfall, canopy_evap, runoff, omega_leaf);
    } else {
        update_daily_water_struct(f, soil_evap, transpiration, et, interception,
                                  throughfall, canopy_evap, runoff);
    }

    return;
}

void calculate_water_balance_hydraulics(control *c, fluxes *f, met *m,
                                        params *p, state *s, int daylen,
                                        double trans_leaf, double omega_leaf,
                                        double rnet_leaf) {
    /*

    Calculate the water balance (including all water fluxes).
        - we are using all the hydraulics instead

    Parameters:
    ----------
    control : structure
        control structure
    fluxes : structure
        fluxes structure
    met : structure
        meteorological drivers structure
    params : structure
        parameters structure
    day : int
        project day. (Dummy argument, only passed for daily model)
    daylen : double
        length of day in hours. (Dummy argument, only passed for daily model)
    trans_leaf : double
        leaf transpiration (Dummy argument, only passed for sub-daily model)
    omega_leaf : double
        decoupling coefficient (Dummy argument, only passed for sub-daily model)
    rnet_leaf : double
        total canopy rnet (Dummy argument, only passed for sub-daily model)

    */
    int    i, rr;
    double soil_evap, et, interception, runoff, conv, transpiration, net_rad;
    double SEC_2_DAY, DAY_2_SEC, transpiration_am, transpiration_pm, gs_am;
    double gs_pm, LE_am, LE_pm, ga_am, ga_pm, net_rad_am, net_rad_pm, omega_am;
    double gpp_am, gpp_pm, omega_pm, throughfall, canopy_evap;
    double water_content;

    /* assume that water infiltrates within a timestep */
    double surface_watermm = 0.0;

    calc_soil_conductivity(f, p, s);
    calc_soil_water_potential(f, p, s);
    calc_soil_root_resistance(f, p, s);
    calc_water_uptake_per_layer(f, p, s);

    /* calculate potential canopy evap rate, this may be reduced later
       depending on canopy water storage */
    canopy_evap = calc_canopy_evaporation(m, p, s, rnet_leaf);

    /* mol m-2 s-1 to mm d-1 */
    conv = MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_HLFHR;
    canopy_evap *= conv;
    calc_interception(c, m, p, f, s, &throughfall, &interception,
                      &canopy_evap);

    net_rad = calc_net_radiation(p, m->sw_rad, m->tair);
    soil_evap = calc_soil_evaporation(m, p, s, net_rad);
    soil_evap *= MOLE_WATER_2_G_WATER * G_TO_KG * SEC_2_HLFHR;

    /* mol m-2 s-1 to mm/30 min */
    transpiration = trans_leaf * MOLE_WATER_2_G_WATER * G_TO_KG * \
                    SEC_2_HLFHR;

    /* determine water loss in upper layers due to evaporation */
    calc_wetting_layers(f, p, s, soil_evap, surface_watermm);

    /*  From which layer is evap water withdrawn? */
    if (s->dry_thick < s->thickness[0]) {
        rr = 1;     /* The dry zone does not extend beneath the top layer */
    } else {
        rr = 2;     /* The dry zone does extend beneath the top layer */
    }

    /* determine water loss in upper layers due to evaporation */
    if (soil_evap > 0.0) {
      /* Evaporation (m 30min-1) */
      f->water_loss[rr] += soil_evap * MM_TO_M;
    } /* ignoring water gain due to due formation...


    /* Determing water loss from each layer due to transpiration */
    for (i = 0; i < s->rooted_layers; i++) {
        f->water_loss[i] += (transpiration * MM_TO_M) * f->fraction_uptake[i];
    }

    /*
    ** determines water movement between soil layers to due drainage
    ** down the profile
    */
    for (i = 0; i < p->n_layers; i++) {
        calc_soil_balance(f, p, s, i);
    }

    /*
    ** how much surface water infiltrantes the first soil layer in the current
    ** step. Water which does not infiltrate in a single step is considered
    ** runoff
    */
    runoff = calc_infiltration(f, p, s, surface_watermm);
    calc_soil_water_potential(f, p, s);
    calc_soil_root_resistance(f, p, s);

    for (i = 0; i < p->n_layers; i++) {
        /* water content of layer (m or tonnes.m-2) */
        water_content = s->water_frac[i] * s->thickness[i];

        /*
        ** Net change in water content (m or tonnes.m-2);
        ** max condition to ensure
        */
        water_content = MAX(0.0, water_content + \
                                f->water_gain[i] + \
                                f->ppt_gain[i] - \
                                f->water_loss[i]);
        /* Determine new total water content of layer (m or tonnes.m-2) */
        s->water_frac[i] = water_content / s->thickness[i];

    }
    exit(1);

    et = transpiration + soil_evap + canopy_evap;

    sum_hourly_water_fluxes(f, soil_evap, transpiration, et, interception,
                            throughfall, canopy_evap, runoff, omega_leaf);
}


void update_water_storage(control *c, fluxes *f, params *p, state *s,
                          double throughfall, double interception,
                          double canopy_evap, double *transpiration,
                          double *soil_evap, double *et, double *runoff) {
    /*
        Calculate top soil, root zone plant available water & runoff.

        NB. et, transpiration & soil evap may all be adjusted in
        if we don't have sufficient water

    */
    double trans_frac, transpiration_topsoil, transpiration_root, previous,
           delta_topsoil, topsoil_loss;

    /* reduce trans frac extracted from the topsoil if the layer is dry */
    trans_frac = p->fractup_soil * s->wtfac_topsoil;
    transpiration_topsoil = trans_frac * *transpiration;

    /* Top soil layer */
    previous = s->pawater_topsoil;
    topsoil_loss = transpiration_topsoil + *soil_evap;
    s->pawater_topsoil += throughfall - topsoil_loss;

    if (s->pawater_topsoil < 0.0) {
        s->pawater_topsoil = 0.0;

        /* use any available water to meet soil evap demands first */
        if (*soil_evap > previous) {
            *soil_evap = previous;
            transpiration_topsoil = 0.0;
        } else {
            *soil_evap = previous;
            transpiration_topsoil = previous - *soil_evap;
        }
    } else if (s->pawater_topsoil > p->wcapac_topsoil) {
        s->pawater_topsoil = p->wcapac_topsoil;
    }
    delta_topsoil = MAX(0.0, s->pawater_topsoil + topsoil_loss - previous);

    /* Account for water lost from the topsoil from throughfall to rootzone */
    throughfall -= delta_topsoil;
    throughfall = MAX(0.0, throughfall);

    /* Account for transpiration already extracted from the topsoil */
    transpiration_root = *transpiration - transpiration_topsoil;

    /* Root zone */
    previous = s->pawater_root;
    s->pawater_root += throughfall - transpiration_root;

    /* calculate runoff and remove any excess from rootzone */
    if (s->pawater_root > p->wcapac_root) {
        *runoff = s->pawater_root - p->wcapac_root;
        s->pawater_root -= *runoff;
    } else if (s->pawater_root < 0.0) {
        s->pawater_root = 0.0;
        transpiration_root = previous;
        *runoff = 0.0;
    } else {
        *runoff = 0.0;
    }

    /* Update transpiration & et accounting for the actual available water */
    *transpiration = transpiration_topsoil + transpiration_root;
    *et = *transpiration + *soil_evap + canopy_evap;
    s->delta_sw_store = s->pawater_root - previous;

    /* calculated at the end of the day for sub_daily */
    if (! c->sub_daily) {
        if (c->water_stress) {
            /* Calculate the soil moisture availability factors [0,1] in the
               topsoil and the entire root zone */
            calculate_soil_water_fac(c, p, s);
        } else {
            /* really this should only be a debugging option! */
            s->wtfac_topsoil = 1.0;
            s->wtfac_root = 1.0;
        }
    }

    return;
}


void update_water_storage_recalwb(control *c, fluxes *f, params *p, state *s,
                                  met *m) {
    /* Calculate root and top soil plant available water and runoff.

    Soil drainage is estimated using a "leaky-bucket" approach with two
    soil layers. In reality this is a combined drainage and runoff
    calculation, i.e. "outflow". There is no drainage out of the "bucket"
    soil.

    Returns:
    --------
    outflow : float
        outflow [mm d-1]
    */
    double transpiration_topsil, transpiration_root, previous, delta_topsoil;

    /* reduce transpiration from the top soil if it is dry */
    transpiration_topsil = p->fractup_soil * s->wtfac_topsoil * f->transpiration;

    /* Total soil layer */
    previous = s->pawater_topsoil;
    s->pawater_topsoil += f->throughfall - transpiration_topsil - f->soil_evap;

    if (s->pawater_topsoil < 0.0) {
        s->pawater_topsoil = 0.0;

        /* use any available water to meet soil evap demands first */
        if (f->soil_evap > previous) {
            f->soil_evap = previous;
            transpiration_topsil = 0.0;
        } else {
            f->soil_evap = previous;
            transpiration_topsil = previous - f->soil_evap;
        }
    } else if (s->pawater_topsoil > p->wcapac_topsoil) {
        s->pawater_topsoil = p->wcapac_topsoil;
    }

    delta_topsoil = MAX(0.0, previous - s->pawater_topsoil);


    /* Total root zone */
    previous = s->pawater_root;
    transpiration_root = f->transpiration - transpiration_topsil;
    s->pawater_root += (f->throughfall - delta_topsoil) - transpiration_root;

    /* calculate runoff and remove any excess from rootzone */
    if (s->pawater_root > p->wcapac_root) {
        f->runoff = s->pawater_root - p->wcapac_root;
        s->pawater_root -= f->runoff;
    } else {
        f->runoff = 0.0;
    }

    if (s->pawater_root < 0.0) {
        s->pawater_root = 0.0;
        transpiration_root = previous;
    } else if (s->pawater_root > p->wcapac_root)
        s->pawater_root = p->wcapac_root;

    f->transpiration = transpiration_topsil + transpiration_root;
    f->et = f->transpiration + f->soil_evap + f->canopy_evap;

    s->delta_sw_store = s->pawater_root - previous;

    /* calculated at the end of the day for sub_daily */
    if (c->water_stress) {
        /* Calculate the soil moisture availability factors [0,1] in the
           topsoil and the entire root zone */
        calculate_soil_water_fac(c, p, s);
    } else {
        /* really this should only be a debugging option! */
        s->wtfac_topsoil = 1.0;
        s->wtfac_root = 1.0;
    }


    return;
}

void calc_interception(control *c, met *m, params *p, fluxes *f, state *s,
                       double *throughfall, double *interception,
                       double *canopy_evap) {
    /*
    Estimate canopy interception.

    1. At the day scale using a simple model from Landsberg
    2. At the sub-daily time scale using the logic from CABLE, but ignoring
       canopy drainage e.g. a Rutter type model for the moment.

    Parameters:
    -------
    rain : float
        rainfall [mm d-1]

    References:
    ----------
    * Wang (2011)
    * Landsberg and Sands

    */
    double canopy_spill, canopy_capacity, max_interception;

    if (c->sub_daily) {

        /* Max canopy intercept (mm): BATS-type canopy saturation
           proportional to LAI */
        canopy_capacity = 0.1 * s->lai;

        /* Calculate canopy intercepted rainfall */
        if (m->rain > 0.0) {
            max_interception = MIN(m->rain, canopy_capacity - s->canopy_store);
            *interception = MAX(0.0, max_interception);
            if (m->tair < 0.0) {
                *interception = 0.0;
            }
        } else {
            *interception = 0.0;
        }

        /* Define canopy throughfall */
        *throughfall = m->rain - *interception;

        /* Add canopy interception to canopy storage term */
        s->canopy_store += *interception;


        /* Calculate canopy water storage excess */
        if (s->canopy_store > canopy_capacity) {
            canopy_spill = s->canopy_store - canopy_capacity;
        } else {
            canopy_spill = 0.0;
        }

        /* Move excess canopy water to throughfall */
        *throughfall += canopy_spill;

        /* Update canopy storage term */
        s->canopy_store -= canopy_spill;

        /* remove canopy evap flux */;
        if (s->canopy_store > *canopy_evap) {
            s->canopy_store -= *canopy_evap;
        } else {
            /* reduce evaporation to water available */
            *canopy_evap = s->canopy_store;
            s->canopy_store = 0.0;
        }

    } else {

        if (m->rain > 0.0) {
            /*
            *throughfall  = MAX(0.0, m->rain * p->rfmult - s->lai * p->wetloss);
            *canopy_evap = m->rain - *throughfall;
            *interception = 0.0;
            */

            *canopy_evap = (m->rain * p->intercep_frac * \
                            MIN(1.0, s->lai / p->max_intercep_lai));
            *throughfall = m->rain - f->interception;
            *interception = 0.0;


        } else {
            *canopy_evap = 0.0;
            *throughfall = 0.0;
            *interception = 0.0;
        }
    }

    return;
}

double calc_soil_evaporation(met *m, params *p, state *s, double net_rad) {
    /* Use Penman eqn to calculate top soil evaporation flux at the
    potential rate.

    Soil evaporation is dependent upon soil wetness and plant cover. The net
    radiation term is scaled for the canopy cover passed to this func and
    the impact of soil wetness is accounted for in the wtfac term. As the
    soil dries the evaporation component reduces significantly.

    Key assumptions from Ritchie...

    * When plant provides shade for the soil surface, evaporation will not
    be the same as bare soil evaporation. Wind speed, net radiation and VPD
    will all belowered in proportion to the canopy density. Following
    Ritchie role ofwind, VPD are assumed to be negligible and are therefore
    ignored.

    These assumptions are based on work with crops and whether this holds
    for tree shading where the height from the soil to the base of the
    crown is larger is questionable.

    units = (mm/day)

    References:
    -----------
    * Ritchie, 1972, Water Resources Research, 8, 1204-1213.

    Parameters:
    -----------
    tair : float
        temperature [degC]
    net_rad : float
        net radiation [W m-2]
    press : float
        air pressure [kPa]

    Returns:
    --------
    soil_evap : float
        soil evaporation [mm d-1]

    */
    double lambda, gamma, slope, soil_evap;

    lambda = calc_latent_heat_of_vapourisation(m->tair);
    gamma = calc_pyschrometric_constant(m->press, lambda);
    slope = calc_slope_of_sat_vapour_pressure_curve(m->tair);

    /* mol H20 m-2 s-1 */
    soil_evap = ((slope / (slope + gamma)) * net_rad) / lambda;

    /*
      Surface radiation is reduced by overstory LAI cover. This empirical
      fit comes from Ritchie (1972) and is formed by a fit between the LAI
      of 5 crops types and the fraction of observed net radiation at the
      surface. Whilst the LAI does cover a large range, nominal 0–6, there
      are only 12 measurements and only three from LAI > 3. So this might
      not hold as well for a forest canopy?
      Ritchie 1972, Water Resources Research, 8, 1204-1213.
    */
    if (s->lai > 0.0)
        soil_evap *= exp(-0.398 * s->lai);

    /* reduce soil evaporation if top soil is dry */
    soil_evap *= s->wtfac_topsoil;

    return (soil_evap);
}

double calc_canopy_evaporation(met *m, params *p, state *s, double rnet) {
    /* Use Penman eqn to calculate evaporation flux at the potential rate for
    canopy evaporation

    units = (mm/day)

    Parameters:
    -----------
    tair : float
        temperature [degC]
    net_rad : float
        net radiation [W m-2]
    press : float
        air pressure [kPa]

    Returns:
    --------
    pot_evap : float
        evaporation [mm d-1]

    */
    double lambda, gamma, slope, arg1, arg2, pot_evap, LE, ga;

    ga = canopy_boundary_layer_conduct(p, s->canht, m->wind, m->press, m->tair);
    lambda = calc_latent_heat_of_vapourisation(m->tair);
    gamma = calc_pyschrometric_constant(m->press, lambda);
    slope = calc_slope_of_sat_vapour_pressure_curve(m->tair);

    arg1 = slope * rnet + m->vpd * ga * CP * MASS_AIR;
    arg2 = slope + gamma;
    LE = arg1 / arg2; /* W m-2 */
    pot_evap = LE / lambda; /* mol H20 m-2 s-1 */

    return (pot_evap);
}


double calc_net_radiation(params *p, double sw_rad, double tair) {

    double net_rad, net_lw;

    /* Net loss of long-wave radn, Monteith & Unsworth '90, pg 52, eqn 4.17 */
    net_lw = 107.0 - 0.3 * tair;            /* W m-2 */

    /* Net radiation recieved by a surf, Monteith & Unsw '90, pg 54 eqn 4.21
        - note the minus net_lw is correct as eqn 4.17 is reversed in
          eqn 4.21, i.e Lu-Ld vs. Ld-Lu
        - NB: this formula only really holds for cloudless skies!
        - Bounding to zero, as we can't have negative soil evaporation, but you
          can have negative net radiation.
        - units: W m-2
    */
    net_rad = MAX(0.0, (1.0 - p->albedo) * sw_rad - net_lw);

    return (net_rad);
}


void penman_canopy_wrapper(params *p, state *s, double press, double vpd,
                           double tair, double wind, double rnet, double ca,
                           double gpp, double *ga, double *gsv,
                           double *transpiration, double *LE, double *omega) {
    /*
        Calculates transpiration at the canopy scale (or big leaf) using the
        Penman-Monteith

        Parameters:
        ----------
        parms : structure
            parameters
        state : structure
            state variables
        press : float
            atmospheric pressure (Pa)
        vpd : float
            vapour pressure deficit of air (Pa)
        tair : float
            air temperature (deg C)
        wind : float
            wind speed (m s-1)
        rnet : float
            net radiation (J m-2 s-1)
        ca : float
            ambient CO2 concentration (umol mol-1)
        gpp : float
            gross primary productivity (umol m-2 s-1)
        ga : float
            canopy scale boundary layer conductance (mol m-2 s-1; returned)
        gsv : float
            stomatal conductance to H20 (mol m-2 s-1; returned)
        transpiration : float
            transpiration (mol H2O m-2 s-1; returned)
        LE : float
            latent heat flux (W m-2; returned)


    */
    double gv, gsc, epsilon, slope, gamma, lambda;

    /* stomtal conductance to CO2 */
    gsc = calc_stomatal_conductance(p, s, vpd, ca, gpp);

    /* stomtal conductance to H2O */
    *gsv = GSVGSC * gsc;

    *ga = canopy_boundary_layer_conduct(p, s->canht, wind, press, tair);

    /* Total leaf conductance to water vapour */
    gv = 1.0 / (1.0 / *gsv + 1.0 / *ga);

    lambda = calc_latent_heat_of_vapourisation(tair);
    gamma = calc_pyschrometric_constant(press, lambda);
    slope = calc_slope_of_sat_vapour_pressure_curve(tair);

    penman_monteith(press, vpd, rnet, slope, lambda, gamma, ga, &gv,
                    transpiration, LE);

    /* Calculate decoupling coefficient (McNaughton and Jarvis 1986) */
    epsilon = slope / gamma;
    *omega = (1.0 + epsilon) / (1.0 + epsilon + *ga / *gsv);

    return;
}

void penman_leaf_wrapper(met *m, params *p, state *s, double tleaf, double rnet,
                         double gsc, double *transpiration, double *LE,
                         double *gbc, double *gh, double *gv, double *omega) {
    /*
        Calculates transpiration by leaves using the Penman-Monteith

        Parameters:
        ----------
        press : float
            atmospheric pressure (Pa)
        rnet : float
            net radiation (J m-2 s-1)
        vpd : float
            vapour pressure deficit of air (Pa)
        tair : float
            air temperature (deg C)
        transpiration : float
            transpiration (mol H2O m-2 s-1) (returned)
        LE : float
            latent heat flux, W m-2 (returned)


    */
    double slope, epsilon, lambda, gradn, gbhu, gbhf, gbh, gbv, gsv, gamma;

    /* Radiation conductance (mol m-2 s-1) */
    gradn = calc_radiation_conductance(m->tair);

    /* Boundary layer conductance for heat - single sided, forced
       convection (mol m-2 s-1) */
    gbhu = calc_bdn_layer_forced_conduct(m->tair, m->press, m->wind,
                                         p->leaf_width);

    /* Boundary layer conductance for heat - single sided, free convection */
    gbhf = calc_bdn_layer_free_conduct(m->tair, tleaf, m->press, p->leaf_width);

    /* Total boundary layer conductance for heat */
    gbh = gbhu + gbhf;

    /* Total conductance for heat - two-sided */
    *gh = 2.0 * (gbh + gradn);

    gbv = GBVGBH * gbh;
    gsv = GSVGSC * gsc;

    /* Total leaf conductance to water vapour */
    *gv = (gbv * gsv) / (gbv + gsv);
    *gbc = gbh / GBHGBC;

    lambda = calc_latent_heat_of_vapourisation(m->tair);
    gamma = calc_pyschrometric_constant(m->press, lambda);
    slope = calc_slope_of_sat_vapour_pressure_curve(m->tair);

    penman_monteith(m->press, m->vpd, rnet, slope, lambda, gamma, gh, gv,
                    transpiration, LE);

    /* Calculate decoupling coefficient (McNaughton and Jarvis 1986) */
    epsilon = slope / gamma;
    *omega = (1.0 + epsilon) / (1.0 + epsilon + gbv / gsv);

    return;
}

void penman_monteith(double press, double vpd, double rnet, double slope,
                     double lambda, double gamma, double *gh, double *gv,
                     double *transpiration, double *LE) {
    /*
        Calculates transpiration using the Penman-Monteith

        Parameters:
        ----------
        press : float
            atmospheric pressure (Pa)
        vpd : float
            vapour pressure deficit of air (Pa)
        rnet : float
            net radiation (J m-2 s-1)
        slope : float
            slope of VPD/T curve, Pa K-1
        lambda : flot
            latent heat of water at air T, J mol-1
        gamma : float
            psychrometric constant, J mol-1
        gh : float
            boundary layer conductance to heat (free & forced & radiative
            components), mol m-2 s-1
        gv : float
            conductance to water vapour (stomatal & bdry layer components),
            mol m-2 s-1
        transpiration : float
            transpiration (mol H2O m-2 s-1; returned)
        LE : float
            latent heat flux (W m-2; returned)
        omega : float
            decoupling coefficient (unitless; returned)

        References:
        ------------
        * Medlyn et al. (2007), Tree Physiology, 27, 1687-1699.
    */
    double arg1, arg2;

    if (*gv > 0.0) {
        arg1 = slope * rnet + vpd * *gh * CP * MASS_AIR;
        arg2 = slope + gamma * *gh / *gv;
        *LE = arg1 / arg2; /* W m-2 */
        *transpiration = *LE / lambda; /* mol H20 m-2 s-1 */
    } else {
        *transpiration = 0.0;
    }

    /* Should not be negative - not sure gv>0.0 catches it as g0 = 1E-09? */
    *transpiration = MAX(0.0, *transpiration);

    return;
}

double calc_stomatal_conductance(params *p, state *s, double vpd, double Ca,
                                 double gpp) {
    /*
        Calculate stomatal conductance using Belinda's model. For the medlyn
        model this is already in conductance to CO2, so the 1.6 from the
        corrigendum to Medlyn et al 2011 is missing here

    References:
    -----------
    For conversion factor for conductance see...
    * Jones (1992) Plants and microclimate, pg 56 + Appendix 3
    * Diaz et al (2007) Forest Ecology and Management, 244, 32-40.

    Stomatal Model:
    * Medlyn et al. (2011) Global Change Biology, 17, 2134-2144.
    **Note** Corrigendum -> Global Change Biology, 18, 3476.

    Parameters:
    -----------
    g1 : float
        slope
    wtfac : float
        water availability scaler [0,1]
    vpd : float
        vapour pressure deficit (Pa)
    Ca : float
        atmospheric co2 [umol mol-1]
    gpp : float
        photosynthesis at the canopy scale (umol m-2 s-1)

    Returns:
    --------
    gs : float
        stomatal conductance (mol CO2 m-2 s-1)
    */
    double gs_over_a, g1, g0, gsc;

    g1 = p->g1 * s->wtfac_root;
    g0 = 0.0; /* p->g0; */
    gs_over_a = (1.0 + g1 / sqrt(vpd * PA_2_KPA)) / Ca;
    gsc = MAX(g0, g0 + gs_over_a * gpp);

    /* mol m-2 s-1 */
    return (gsc);

}





double canopy_boundary_layer_conduct(params *p, double canht, double wind,
                                     double press, double tair) {
    /*  Canopy boundary layer conductance, ga (from Jones 1992 p 68)


    Notes:
    ------
    'Estimates of ga for pine canopies from LAI of 3 to 6 vary from
    3.5 to 1.1 mol m-2 s-1  (Kelliher et al., 1993; Juang et al., 2007).'
    Drake et al, 2010, 17, pg. 1526.

    References:
    ------------
    * Jones 1992, pg. 67-8.
    * Monteith and Unsworth (1990), pg. 248. Note this in the inverted form
      of what is in Monteith (ga = 1 / ra)
    * Allen et al. (1989) pg. 651.
    * Gash et al. (1999) Ag forest met, 94, 149-158.

    Parameters:
    -----------
    params : p
        parameters structure
    canht : float
        canopy height (m)
    wind : float
        wind speed (m s-1)
    press : float
        atmospheric pressure (Pa)
    tair : float
        air temperature (deg C)

    Returns:
    --------
    ga : float
        canopy boundary layer conductance (mol m-2 s-1)
    */

    /* z0m roughness length governing momentum transfer [m] */
    double z0m, z0h, d, arg1, arg2, arg3, ga, cmolar;
    double vk = 0.41;

    /* Convert from mm s-1 to mol m-2 s-1 */
    cmolar = press / (RGAS * (tair + DEG_TO_KELVIN));

    /* roughness length for momentum */
    z0m = p->dz0v_dh * canht;

    /*
       z0h roughness length governing transfer of heat and vapour [m]
      *Heat tranfer typically less efficent than momentum transfer. There is
       a lot of variability in values quoted for the ratio of these two...
       JULES uses 0.1, Campbell and Norman '98 say z0h = z0m / 5. Garratt
       and Hicks, 1973/ Stewart et al '94 say z0h = z0m / 7. Therefore for
       the default I am following Monteith and Unsworth, by setting the
       ratio to be 1, the code below is identical to that on page 249,
       eqn 15.7
    */
    z0h = p->z0h_z0m * z0m;

    /* zero plan displacement height [m] */
    d = p->displace_ratio * canht;

    arg1 = (vk * vk) * wind;
    arg2 = log((canht - d) / z0m);
    arg3 = log((canht - d) / z0h);

    ga = (arg1 / (arg2 * arg3)) * cmolar;

    return (ga);
}

double calc_radiation_conductance(double tair) {
    /*  Returns the 'radiation conductance' at given temperature.

        Units: mol m-2 s-1

        References:
        -----------
        * Formula from Ying-Ping's version of Maestro, cf. Wang and Leuning
          1998, Table 1,
        * See also Jones (1992) p. 108.
        * And documented in Medlyn 2007, equation A3, although I think there
          is a mistake. It should be Tk**3 not Tk**4, see W & L.
    */
    double grad;
    double Tk;

    Tk = tair + DEG_TO_KELVIN;
    grad = 4.0 * SIGMA * (Tk * Tk * Tk) * LEAF_EMISSIVITY / (CP * MASS_AIR);

    return (grad);
}

double calc_bdn_layer_forced_conduct(double tair, double press, double wind,
                                     double leaf_width) {
    /*
        Boundary layer conductance for heat - single sided, forced convection
        (mol m-2 s-1)
        See Leuning et al (1995) PC&E 18:1183-1200 Eqn E1
    */
    double cmolar, Tk, gbh;

    Tk = tair + DEG_TO_KELVIN;
    cmolar = press / (RGAS * Tk);
    gbh = 0.003 * sqrt(wind / leaf_width) * cmolar;

    return (gbh);
}

double calc_bdn_layer_free_conduct(double tair, double tleaf, double press,
                                   double leaf_width) {
    /*
        Boundary layer conductance for heat - single sided, free convection
        (mol m-2 s-1)
        See Leuning et al (1995) PC&E 18:1183-1200 Eqns E3 & E4
    */
    double cmolar, Tk, gbh, grashof, leaf_width_cubed;

    Tk = tair + DEG_TO_KELVIN;
    cmolar = press / (RGAS * Tk);
    leaf_width_cubed = leaf_width * leaf_width * leaf_width;

    if (float_eq((tleaf - tair), 0.0)) {
        gbh = 0.0;
    } else {
        grashof = 1.6E8 * fabs(tleaf - tair) * leaf_width_cubed;
        gbh = 0.5 * DHEAT * pow(grashof, 0.25) / leaf_width * cmolar;
    }

    return (gbh);
}


double calc_slope_of_sat_vapour_pressure_curve(double tair) {
    /*
        Constant slope in Penman-Monteith equation

        Parameters:
        -----------
        tavg : float
            average daytime temperature

        Returns:
        --------
        slope : float
            slope of saturation vapour pressure curve [Pa K-1]

    */
    double arg1, arg2, slope;

    /* Const slope in Penman-Monteith equation  (Pa K-1) */
    arg1 = calc_sat_water_vapour_press(tair + 0.1);
    arg2 = calc_sat_water_vapour_press(tair);
    slope = (arg1 - arg2) / 0.1;

    return (slope);
}


double calc_pyschrometric_constant(double press, double lambda) {
    /* Psychrometric constant ratio of specific heat of moist air at
    a constant pressure to latent heat of vaporisation.

    Parameters:
    -----------
    press : float
        air pressure (Pa)
    lambda : float
         latent heat of water vaporization (J mol-1)


    Returns:
    --------
    gamma : float
        pyschrometric constant [Pa K-1]

    */
    return ( CP * MASS_AIR * press / lambda );

}

double calc_latent_heat_of_vapourisation(double tair) {
    /*   Latent heat of water vapour at air temperature

        Returns:
        -----------
        lambda : float
            latent heat of water vaporization [J mol-1]
    */
    return ( (H2OLV0 - 2.365E3 * tair) * H2OMW );

}

double calc_sat_water_vapour_press(double tac) {
    /*
        Calculate saturated water vapour pressure (Pa) at
        temperature TAC (Celsius). From Jones 1992 p 110 (note error in
        a - wrong units)
    */
    return (613.75 * exp(17.502 * tac / (240.97 + tac)));
}



double *get_soil_fracs(char *soil_type) {
    /*
     * Based on Table 2 in Cosby et al 1984, page 2.
     * Fractions of silt, sand and clay (in that order)
     */
    double *fsoil = malloc(3 * sizeof(double));

    if (strcmp(soil_type, "sand") == 0) {
        fsoil[0] = 0.05;
        fsoil[1] = 0.92;
        fsoil[2] = 0.03;
    } else if (strcmp(soil_type, "loamy_sand") == 0) {
        fsoil[0] = 0.12;
        fsoil[1] = 0.82;
        fsoil[2] = 0.06;
    } else if (strcmp(soil_type, "sandy_loam") == 0) {
        fsoil[0] = 0.32;
        fsoil[1] = 0.58;
        fsoil[2] = 0.1;
    } else if (strcmp(soil_type, "loam") == 0) {
        fsoil[0] = 0.39;
        fsoil[1] = 0.43;
        fsoil[2] = 0.18;
    } else if (strcmp(soil_type, "silty_loam") == 0) {
        fsoil[0] = 0.7;
        fsoil[1] = 0.17;
        fsoil[2] = 0.13;
    } else if (strcmp(soil_type, "sandy_clay_loam") == 0) {
        fsoil[0] = 0.15;
        fsoil[1] = 0.58;
        fsoil[2] = 0.27;
    } else if (strcmp(soil_type, "clay_loam") == 0) {
        fsoil[0] = 0.34;
        fsoil[1] = 0.32;
        fsoil[2] = 0.34;
    } else if (strcmp(soil_type, "silty_clay_loam") == 0) {
        fsoil[0] = 0.56;
        fsoil[1] = 0.1;
        fsoil[2] = 0.34;
    } else if (strcmp(soil_type, "sandy_clay") == 0) {
        fsoil[0] = 0.06;
        fsoil[1] = 0.52;
        fsoil[2] = 0.42;
    } else if (strcmp(soil_type, "silty_clay") == 0) {
        fsoil[0] = 0.47;
        fsoil[1] = 0.06;
        fsoil[2] = 0.47;
    } else if (strcmp(soil_type, "clay") == 0) {
        fsoil[0] = 0.2;
        fsoil[1] = 0.22;
        fsoil[2] = 0.58;
    } else {
        prog_error("Could not understand soil type", __LINE__);
    }

    return (fsoil);
}

void get_soil_params(char *soil_type, double *c_theta, double *n_theta) {
    /* For a given soil type, get the parameters for the soil
    moisture availability based on Landsberg and Waring, with updated
    parameters from Landsberg and Sands (2011), pg 190, Table 7.1

    Table also has values from Saxton for soil texture, perhaps makes more
    sense to use those than Cosby? Investigate?

    Reference
    ---------
    * Landsberg and Sands (2011) Physiological ecology of forest production.
    * Landsberg and Waring (1997) Forest Ecology & Management, 95, 209-228.
    * Clapp & Hornberger (1978) Water Resources Research, 14, 601–604.
    */

    if (strcmp(soil_type, "clay") == 0) {
        *c_theta = 0.4;
        *n_theta = 3.0;
    } else if (strcmp(soil_type, "clay_loam") == 0) {
        *c_theta = 0.5;
        *n_theta = 5.0;
    } else if (strcmp(soil_type, "loam") == 0) {
        *c_theta = 0.55;
        *n_theta = 6.0;
    } else if (strcmp(soil_type, "loamy_sand") == 0) {
        *c_theta = 0.65;
        *n_theta = 8.0;
    } else if (strcmp(soil_type, "sand") == 0) {
        *c_theta = 0.7;
        *n_theta = 9.0;
    } else if (strcmp(soil_type, "sandy_clay") == 0) {
        *c_theta = 0.45;
        *n_theta = 4.0;
    } else if (strcmp(soil_type, "sandy_clay_loam") == 0) {
        *c_theta = 0.525;
        *n_theta = 5.5;
    } else if (strcmp(soil_type, "sandy_loam") == 0) {
        *c_theta = 0.6;
        *n_theta = 7.0;
    } else if (strcmp(soil_type, "silt") == 0) {
        *c_theta = 0.625;
        *n_theta = 7.5;
    } else if (strcmp(soil_type, "silty_clay") == 0) {
        *c_theta = 0.425;
        *n_theta = 3.5;
    } else if (strcmp(soil_type, "silty_clay_loam") == 0) {
        *c_theta = 0.475;
        *n_theta = 4.5;
    } else if (strcmp(soil_type, "silty_loam") == 0) {
        *c_theta = 0.575;
        *n_theta = 6.5;
    } else {
        prog_error("There are no parameters for your soil type", __LINE__);
    }

    return;
}

void calc_soil_params(double *fsoil, double *theta_fc, double *theta_wp,
                      double *theta_sp, double *b, double *psi_sat_mpa) {
    /* Cosby parameters for use within the Clapp Hornberger soil hydraulics
    scheme are calculated based on the texture components of the soil.

    NB: Cosby et al were ambiguous in their paper as to what log base to
    use.  The correct implementation is base 10, as below.

    Parameters:
    ----------
    fsoil : list
        fraction of silt, sand, and clay (in that order

    Returns:
    --------
    theta_fc : float
        volumetric soil water concentration at field capacity
    theta_wp : float
        volumetric soil water concentration at the wilting point

    */
    /* soil suction of 3.364m and 152.9m, or equivalent of -0.033 & -1.5 MPa */
    double pressure_head_wilt = -152.9;
    double pressure_head_crit = -3.364;
    double psi_sat;

    /* *Note* subtle unit change to be consistent with fractions as opposed
     * to percentages of sand, silt, clay, e.g. I've changed the slope in
     * the "b" Clapp paramter from 0.157 to 15.7
     *
     * Also Cosby is unclear about which log base were used. 'Generally' now
     * assumed that logarithms to the base 10
     */

    /* Clapp Hornberger exponent [-] */
    *b = 3.1 + 15.7 * fsoil[CLAY] - 0.3 * fsoil[SAND];

    /*
     * soil matric potential at saturation, taking inverse of log (base10)
     * units = m
     */
    psi_sat = CM_2_M * -(pow(10.0, (1.54 - 0.95 * fsoil[SAND] +\
              0.63 * fsoil[SILT])));
    *psi_sat_mpa = psi_sat * METER_OF_HEAD_TO_MPA;

    /* volumetric soil moisture concentrations at the saturation point */
    *theta_sp = 0.505 - 0.037 * fsoil[CLAY] - 0.142 * fsoil[SAND];

    /*
     * volumetric soil moisture concentrations at the wilting point
     * assumed to equal suction of -1.5 MPa or a depth of water of 152.9 m
     */
    *theta_wp = *theta_sp * pow((psi_sat / pressure_head_wilt), (1.0 / *b));

    /*
     * volumetric soil moisture concentrations at field capacity assumed to
     * equal a suction of -0.0033 MPa or a depth of water of 3.364 m
     */
    *theta_fc = *theta_sp * pow((psi_sat / pressure_head_crit), (1.0 / *b));

    return;

}

void calculate_soil_water_fac(control *c, params *p, state *s) {
    /* Estimate a relative water availability factor [0..1]

    A drying soil results in physiological stress that can induce stomatal
    closure and reduce transpiration. Further, N mineralisation depends on
    top soil moisture.

    s->qs = 0.2 in SDGVM

    References:
    -----------
    * Landsberg and Waring (1997) Forest Ecology and Management, 95, 209-228.
      See --> Figure 2.
    * Egea et al. (2011) Agricultural Forest Meteorology, 151, 1370-1384.

    But similarly see:
    * van Genuchten (1981) Soil Sci. Soc. Am. J, 44, 892--898.
    * Wang and Leuning (1998) Ag Forest Met, 91, 89-111.

    * Pepper et al. (2008) Functional Change Biology, 35, 493-508

    Returns:
    --------
    wtfac_topsoil : float
        water availability factor for the top soil [0,1]
    wtfac_root : float
        water availability factor for the root zone [0,1]
    */

    double moisture_ratio_topsoil, moisture_ratio_root;
    /*double psi_swp_topsoil;*/

    if (c->sw_stress_model == 0) {
        /* JULES type model, see Egea et al. (2011) */
        s->wtfac_topsoil = calc_beta(s->pawater_topsoil, p->topsoil_depth,
                                     p->theta_fc_topsoil, p->theta_wp_topsoil,
                                     p->qs);

        s->wtfac_root = calc_beta(s->pawater_root, p->rooting_depth,
                                     p->theta_fc_root, p->theta_wp_root,
                                     p->qs);

    } else if (c->sw_stress_model == 1) {
        /* Landsberg and Waring, (1997) */
        moisture_ratio_topsoil = s->pawater_topsoil / p->wcapac_topsoil;
        moisture_ratio_root = s->pawater_root / p->wcapac_root;

        s->wtfac_topsoil = calc_sw_modifier(moisture_ratio_topsoil,
                                            p->ctheta_topsoil,
                                            p->ntheta_topsoil);
        s->wtfac_root = calc_sw_modifier(moisture_ratio_root, p->ctheta_root,
                                         p->ntheta_root);

    } else if (c->sw_stress_model == 2) {
        /*
            Zhou et al.(2013) Agricultural & Forest Met. 182–183, 204–214
            Assuming that overnight 􏰀pre-dawn leaf water potential =
            pre-dawn soil water potential.
        */
        fprintf(stderr, "Zhou model not implemented\n");
        exit(EXIT_FAILURE);
        /*
        s->wtfac_topsoil = exp(p->g1_b * s->psi_s_topsoil);
        s->wtfac_root = exp(p->g1_b * s->psi_s_root);

        ! SW modifier for Vcmax (non-stomatal limitation)
        s->wtfac_topsoil_ns = (1.0 + exp(p->vcmax_sf * p->vcmax_psi_f)) / \
                              (1.0 + exp(p->vcmax_sf * \
                                        (p->vcmax_psi_f - s->psi_s_topsoil)));
        s->wtfac_root_ns = (1.0 + exp(p->vcmax_sf * p->vcmax_psi_f)) / \
                            (1.0 + exp(p->vcmax_sf * \
                                        (p->vcmax_psi_f - s->psi_s_root)));
        */
    }
    return;
}

double calc_beta(double paw, double depth, double fc, double wp,
                 double exponent) {
    /*
        Soil water modifier, standard JULES/CABLE type approach

        equation 16 in Egea

        Note: we don't need to subtract the wp in the denominator here
              because our plant available water (paw) isn't bounded by
              the wilting point, it reaches zero

        Reference:
        ----------
        * Egea et al. (2011) Agricultural and Forest Meteorology.
    */

    double beta, theta;

    theta = paw / depth;
    beta = pow(theta / (fc - wp), exponent);
    if (beta > fc) {
        beta = 1.0;
    } else if (beta <= wp) {
        beta = 0.0;
    }

    return (beta);
}

double calc_sw_modifier(double theta, double c_theta, double n_theta) {
    /*
        Soil water modifier, equation 2 in Landsberg and Waring.
        Note: "The values of c_theta and n_theta are, nevertheless, chosen
              without specific empirical justification" :)

        Reference:
        ----------
        * Landsberg and Waring (1997) Forest Ecology and Management 95, 209-228.
    */
    return (1.0  / (1.0 + pow(((1.0 - theta) / c_theta), n_theta)));
}


void _calc_soil_water_potential(control *c, params *p, state *s) {
    /*
        Estimate pre-dawn soil water potential from soil water content
    */
    double theta_over_theta_sat, theta;

    /* Soil water potential of topsoil (MPa) */
    theta = (s->pawater_topsoil / p->topsoil_depth) + p->theta_wp_topsoil;
    theta_over_theta_sat = theta / p->theta_sp_topsoil;
    s->psi_s_topsoil = p->psi_sat_topsoil * \
                        pow(theta_over_theta_sat, -p->b_topsoil);

    /* Soil water potential of rootzone (MPa) */
    theta = (s->pawater_root / p->rooting_depth) + p->theta_wp_root;
    theta_over_theta_sat = theta / p->theta_sp_root;
    s->psi_s_root = p->psi_sat_root * pow(theta_over_theta_sat, -p->b_root);
    /*printf("%lf %lf %lf\n", s->psi_s_root, theta, theta_over_theta_sat);
    exit(1);*/
}


void sum_hourly_water_fluxes(fluxes *f, double soil_evap_hlf_hr,
                             double transpiration_hlf_hr, double et_hlf_hr,
                             double interception_hlf_hr,
                             double thoughfall_hlf_hr,
                             double canopy_evap_hlf_hr,
                             double runoff_hlf_hr, double omega_hlf_hr) {

    /* add half hour fluxes to day total store */
    f->soil_evap += soil_evap_hlf_hr;
    f->transpiration += transpiration_hlf_hr;
    f->et += et_hlf_hr;
    f->interception += interception_hlf_hr;
    f->throughfall += thoughfall_hlf_hr;
    f->canopy_evap += canopy_evap_hlf_hr;
    f->runoff += runoff_hlf_hr;
    f->omega += omega_hlf_hr; /* average at the end of hour loop */

    return;
}

void update_daily_water_struct(fluxes *f, double day_soil_evap,
                               double day_transpiration, double day_et,
                               double day_interception, double day_thoughfall,
                               double day_canopy_evap,
                               double day_runoff) {

    /* add half hour fluxes to day total store */
    f->soil_evap = day_soil_evap;
    f->transpiration = day_transpiration;
    f->et = day_et;
    f->interception = day_interception;
    f->throughfall = day_thoughfall;
    f->canopy_evap = day_canopy_evap;
    f->runoff = day_runoff;

    return;
}

void zero_water_day_fluxes(fluxes *f) {

    f->et = 0.0;
    f->soil_evap = 0.0;
    f->transpiration = 0.0;
    f->interception = 0.0;
    f->canopy_evap = 0.0;
    f->throughfall = 0.0;
    f->runoff = 0.0;
    f->gs_mol_m2_sec = 0.0;

    return;
}


void calc_saxton_stuff(params *p, double *fsoil) {
    /*
        Calculate the key parameters of the Saxton equations:
        cond1, 2, 3 and potA, B

        NB: Currently I'm assuming a single texture for the entire root zone,
            we could set it by layer obviously and we probably should...

        Reference:
        ---------
        * Saxton, K. E., Rawls, W. J., Romberger, J. S. & Papendick, R. I.
          (1986) Estimating generalized soil-water characteristics from texture.
          Soil Science Society of America Journal, 90, 1031-1036.
    */
    int    i;
    double mult1 = 100.0;
    double mult2 = 2.778E-6;
    double mult3 = 1000.0;
    double A = -4.396;
    double B = -0.0715;
    double CC = -4.880E-4;
    double D = -4.285E-5;
    double E = -3.140;
    double F = -2.22E-3;
    double G = -3.484E-5;
    double H = 0.332;
    double J = -7.251E-4;
    double K = 0.1276;
    double P = 12.012;
    double Q = -7.551E-2;
    double R = -3.895;
    double T = 3.671e-2;
    double U = -0.1103;
    double V = 8.7546E-4;
    double x1 = 0.1;
    double x2 = 0.7;
    double tol = 0.0001;
    double dummy = 0.0;
    double sand = fsoil[SAND] * 100.0;
    double clay = fsoil[CLAY] * 100.0;

    p->potA = malloc(p->n_layers * sizeof(double));
    if (p->potA == NULL) {
        fprintf(stderr, "malloc failed allocating Saxton's potA\n");
        exit(EXIT_FAILURE);
    }

    p->potB = malloc(p->n_layers * sizeof(double));
    if (p->potB == NULL) {
        fprintf(stderr, "malloc failed allocating Saxton's potB\n");
        exit(EXIT_FAILURE);
    }

    p->cond1 = malloc(p->n_layers * sizeof(double));
    if (p->cond1 == NULL) {
        fprintf(stderr, "malloc failed allocating Saxton's cond1\n");
        exit(EXIT_FAILURE);
    }

    p->cond2 = malloc(p->n_layers * sizeof(double));
    if (p->cond1 == NULL) {
        fprintf(stderr, "malloc failed allocating Saxton's cond2\n");
        exit(EXIT_FAILURE);
    }

    p->cond3 = malloc(p->n_layers * sizeof(double));
    if (p->cond1 == NULL) {
        fprintf(stderr, "malloc failed allocating Saxton's cond3\n");
        exit(EXIT_FAILURE);
    }

    p->porosity = malloc(p->n_layers * sizeof(double));
    if (p->porosity == NULL) {
        fprintf(stderr, "malloc failed allocating porosity\n");
        exit(EXIT_FAILURE);
    }

    p->field_capacity = malloc(p->n_layers * sizeof(double));
    if (p->field_capacity == NULL) {
        fprintf(stderr, "malloc failed allocating field_capacity\n");
        exit(EXIT_FAILURE);
    }

    /*
    ** As we aren't currently changing texture by layer, the loop is redundant,
    ** but it is probably best to leave it under the assumption we change
    ** this later
    */


    for (i = 0; i < p->n_layers; i++) {
        p->potA[i] = exp(A + B * clay + CC * sand * \
                         sand + D * sand * sand * \
                         clay) * 100.0;
        p->potB[i]  = E + F * clay * clay + G * sand * sand * clay;
        p->cond1[i] = mult2;
        p->cond2[i] = P + Q * sand;
        p->cond3[i] = R + T * sand + U * clay + V * clay * clay;

        p->porosity[i] = H + J * sand + K * log10(clay);

        /* field capacity is water content at which SWP = -10 kPa */
        p->field_capacity[i] = zbrent(&saxton_field_capacity, x1, x2, tol,
                                      p->potA[i], p->potB[i],
                                      dummy, dummy, dummy);

    }
    return;
}

double saxton_field_capacity(double xval, double potA, double potB,
                             double dummy1, double dummy2, double dummy3) {
    /* field capacity calculations for saxton eqns */

    double air_entry_swp = 10.0; /* (kPa) */
    double MPa_2_kPa = 1000.0;
    double swp;

    /* calculate the soil water potential (MPa) */
    swp = -0.001 * potA * pow(xval, potB);
    return (MPa_2_kPa * swp + air_entry_swp);
}

void calc_soil_conductivity(fluxes *f, params *p, state *s) {
    /* Soil conductivity (m s-1 ) per layer */
    int    i;
    double arg1, arg2;

    for (i = 0; i < p->n_layers; i++) {

        /* Avoid floating-underflow error */
        if (s->water_frac[i] < 0.05) {
            f->soil_conduct[i] = 1E-30;
        } else {
            arg1 = p->cond1[i];
            arg2 = exp(p->cond2[i] + p->cond3[i] / s->water_frac[i]);
            f->soil_conduct[i] = arg1 * arg2;
        }
    }
    return;
}

void calc_soil_water_potential(fluxes *f, params *p, state *s) {
    /*
        Calculate the SWP (MPa) without updating the water fraction in each
        layer
    */
    int    i;
    double arg1, arg2;

    for (i = 0; i < p->n_layers; i++) {

        if (s->water_frac[i] > 0.0) {
            f->swp[i] = -0.001 * p->potA[i] * pow(s->water_frac[i], p->potB[i]);
        } else {
            f->swp[i] = -9999.0;
        }
    }
    return;

}
void calc_soil_root_resistance(fluxes *f, params *p, state *s) {

    /* head of pressure (MPa/m) */
    double head = 0.009807;

    /* saxton water retention equation are off by default */
    double ab = 1.0;
    double Lsoil, rs, rs2, soilR1, soilR2;
    int    i;

    double root_xsec_area = M_PI * p->root_radius * p->root_radius;

    for (i = 0; i < p->n_layers; i++) {
        /* converts from ms-1 to m2 s-1 MPa-1 */
        Lsoil = f->soil_conduct[i] / head;

        if (Lsoil < 1e-35) {
            /* prevent floating point error */
            f->soilR[i] = 1e35;
        } else {

            rs = sqrt(1.0 / (s->root_length[i] * M_PI));
            rs2 = log(rs / p->root_radius) / \
                     (2.0 * M_PI * s->root_length[i] * s->thickness[i] * Lsoil);

            /* soil resistance, convert from MPa s m2 m-3 to MPa s m2 mmol-1 */
            soilR1 = rs2 * 1E-6 * 18 * 0.001;

            /*
            ** second component of below ground resistance related to root
            ** hydraulics
            */
            soilR2 = p->root_resist / (s->root_mass[i] * s->thickness[i] / ab);
            f->soilR[i] = soilR1 + soilR2; /* MPa s m2 mmol-1 */
        }
    }

    return;
}


void calc_water_uptake_per_layer(fluxes *f, params *p, state *s) {

    /* Figure out which layer water is extracted from */
    int    i;
    double est_evap[p->n_layers];
    double total_est_evap;

    /* Estimate max transpiration from gradient-gravity / soil resistance. */
    total_est_evap = 0.0;
    for (i = 0; i < s->rooted_layers; i++) {
        est_evap[i] = MAX(0.0, (f->swp[i] - p->min_lwp) / f->soilR[i]);
        total_est_evap += est_evap[i];
    }

    /* Water was evaporated from some layers..*/
    s->weighted_swp = 0.0;
    if (total_est_evap > 0.0) {
        for (i = 0; i < p->n_layers; i++) {
            s->weighted_swp += f->swp[i] * est_evap[i];
            /* fraction of water taken from layer */
            f->fraction_uptake[i] = est_evap[i] / total_est_evap;
        }
        s->weighted_swp /= total_est_evap;
    } else {
        /* No water was evaporated */
        f->fraction_uptake[i] = 1.0 / (double)p->n_layers;
    }

    if (f->fraction_uptake[0] > 1 || f->fraction_uptake[0] < 0) {
        fprintf(stderr, "Problem with the uptake fraction\n");
        exit(EXIT_FAILURE);
    }

    return;
}

void calc_wetting_layers(fluxes *f, params *p, state *s, double soil_evap,
                         double surface_watermm) {
    /*
        surface wetting and drying determines thickness of dry layer
        and thus soil evaporation
    */

    double seconds_per_step = 1800.0;
    double dmin = 0.001;
    double airspace = p->porosity[0];
    double min_val, netc, diff;
    int    i, ar1, ar2;

    /*
    ** soil LE should be withdrawn from the wetting layer with the
    ** smallest depth..
    */
    ar1 = 0;
    min_val = 9999.9;
    for (i = 0; i < p->n_layers; i++) {
        if (s->wetting_bot[i] > 0.0) {
            if (s->wetting_bot[i] < min_val) {
                ar1 = i;
                min_val = s->wetting_bot[i];
            }
        }
    }

    /* Calulate the net change in wetting in the top zone */
    netc = (soil_evap * MM_TO_M) / airspace + \
           (surface_watermm * MM_TO_M) / airspace;

    printf("%lf %lf %lf\n", ar1, netc, soil_evap);
    exit(1);
    /* wetting */
    if (netc > 0.0) {
        /*
        ** resaturate the layer if top is dry and recharge is greater
        **  than dry_thick
        */
        if ((netc > s->wetting_top[ar1]) && (s->wetting_top[ar1] > 0.0)) {
            /* extra water to deepen wetting layer */
            diff = netc - s->wetting_top[ar1];
            s->wetting_top[ar1] = 0.0;
            if (ar1 > 0) {
                /* Not in primary layer (primary layer can't extend deeper) */
                s->wetting_bot[ar1] += diff;
            }
            s->dry_thick = dmin;
        } else {
            if (s->wetting_top[ar1] == 0.0) {
                /* surface is already wet, so extend depth of this wet zone */
                if (ar1 > 0) {
                    /* not in primary lay (primary layer can't extend deeper) */
                    s->wetting_bot[ar1] += netc;
                    if (s->wetting_bot[ar1] >= s->wetting_top[ar1-1]) {
                        /* Layers are conterminous.. */
                        s->wetting_top[ar1-1] = s->wetting_top[ar1];
                        s->wetting_top[ar1] = 0.;     /* remove layer */
                        s->wetting_bot[ar1] = 0.;    /* remove layer */
                    }
                }
            } else {
                /* Create a new wetting zone */
                s->wetting_top[ar1+1] = 0.0;
                s->wetting_bot[ar1+1] = netc;
            }
            s->dry_thick = dmin;
        }
    /* drying */
    } else {
        /* Drying increases the wettingtop depth */
        s->wetting_top[ar1] -= netc;

        /* Wetting layer is dried out. */
        if (s->wetting_top[ar1] > s->wetting_bot[ar1]) {
            /* How much more drying is there? */
            diff = s->wetting_top[ar1] - s->wetting_bot[ar1];
            s->wetting_top[ar1] = 0.0;
            s->wetting_bot[ar1] = 0.0;
            ar2 = ar1;
            ar2 -= 1;

            /* Move to deeper wetting layer */
            if (ar2 > 0.0) {
                /* dry out deeper layer */
                s->wetting_top[ar2] += diff;
                s->dry_thick = MAX(dmin, s->wetting_top[ar2]);
            /* no deeper layer */
            } else {
                /* layer 1 is dry */
                s->dry_thick = s->thickness[0];
            }
        } else {
            s->dry_thick = MAX(dmin, s->wetting_top[ar1]);
        }
    }

    if (s->dry_thick == 0.0) {
        fprintf(stderr, "Problem in dry_thick\n");
        exit(EXIT_FAILURE);
    }

    return;
}

double calc_infiltration(fluxes *f, params *p, state *s,
                         double surface_watermm) {
    /*
        Takes surface_watermm and distrubutes it among top layers. Assumes
        total infilatration in timestep.
    */
    int    i;
    double add, wdiff, runoff;

    add = surface_watermm * MM_TO_M;

    for (i = 0; i < p->n_layers; i++) {
        f->ppt_gain[i] = 0.0;
    }

    runoff = 0.0;
    for (i = 0; i < p->n_layers; i++) {
        wdiff = MAX(0.0, (p->porosity[i] - s->water_frac[i]) * \
                          s->thickness[i] - f->water_gain[i] + \
                          f->water_loss[i]);
        if (add > wdiff) {
            f->ppt_gain[i] = wdiff;
            add -= wdiff;
        } else {
            f->ppt_gain[i] = add;
            add = 0.0;
        }

        if (add < 0.0) {
            fprintf(stderr, "Error in infiltration calculation\n");
            exit(EXIT_FAILURE);
        }

        if (add > 0.0) {
            runoff += add;
        }
    }

    return (runoff);
}


void calc_soil_balance(fluxes *f, params *p, state *s, int soil_layer) {
    /* Integrator for soil gravitational drainage */

    int    nbad;                /* N of unsuccessful changes of the step size */
    int    nok;                 /* N of successful changes of the step size */
    int    N = 1;
    double eps = 1.0e-4;        /* precision */
    double h1 = .001;           /* first guess at integrator size */
    double hmin = 0.0;          /* minimum value of the integrator step */
    double x1 = 1.0;             /* initial time */
    double x2 = 2.0;             /* final time */
    /* value affecting the max time interval at which variables should b calc */
    double soilpor = p->porosity[soil_layer];
    double unsat, drain_layer, liquid, new_water_frac, change;

    /* required by odeint */
    int    kmax, kount;
    float *xp, **yp, *ystart, dxsav;

    ystart = vector(1,N);
    xp = vector(1, 200);
    yp = matrix(1,10,1,200);
    kmax = 100;          /* maximum number of iterations  */
    dxsav = (x2 - x1) / 20.0;

    /* unsaturated volume of layer below (m3 m-2) */
    unsat = MAX(0.0, (p->porosity[soil_layer+1] - \
                      s->water_frac[soil_layer+1]) *\
                      s->thickness[soil_layer+1] / s->thickness[soil_layer]);

    /* soil water capacity of the current layer */
    drain_layer = p->field_capacity[soil_layer];
    liquid = s->water_frac[soil_layer];

    /*
    ** initial conditions; i.e. is there liquid water and more
    ** water than layer can hold
    */
    if ( (liquid > 0.0) && (s->water_frac[soil_layer] > drain_layer) ) {

        /* there is liquid water */
        ystart[0] = s->water_frac[soil_layer];

        odeint(ystart, N, x1, x2, eps, h1, hmin, &nok, &nbad,
               soil_water_store, f->soil_conduct[0], unsat, drain_layer);

        new_water_frac = ystart[0];

        /* convert from water fraction to absolute amount (m) */
        change = (s->water_frac[soil_layer] - new_water_frac) * \
                    s->thickness[soil_layer];

        /* update soil layer below with drained liquid */
        f->water_gain[soil_layer+1] += change;
        f->water_loss[soil_layer] += change;
    }

    if (f->water_loss[soil_layer] < 0.0) {
        fprintf(stderr, "waterloss probem in soil_balance\n");
        exit(EXIT_FAILURE);
    }
    free_vector(ystart, 1, N);

    return;
}


void soil_water_store(float time_dummy, float y[], float dydt[],
                      double soil_conductivity, double unsat,
                      double drain_layer) {

    /* determines gravitational water drainage */
    float drainage;

    drainage = soil_conductivity;

    /* gravitational drainage above field_capacity */
    if (y[0] <= drain_layer) {
        drainage = 0.0;
    }

    /* layer below cannot accept more water than unsat */
    if (drainage > unsat) {
        drainage = unsat;
    }
    /* waterloss from this layer */
    dydt[0] = -drainage;
}
