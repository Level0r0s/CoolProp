#ifndef PHASEENVELOPE_H
#define PHASEENVELOPE_H

#include "HelmholtzEOSMixtureBackend.h"
#include "VLERoutines.h"
#include "PhaseEnvelopeRoutines.h"
#include "PhaseEnvelope.h"
#include "CoolPropTools.h"

namespace CoolProp{

void PhaseEnvelopeRoutines::build(HelmholtzEOSMixtureBackend &HEOS)
{
    std::size_t failure_count = 0;
    // Set some imput options
    SaturationSolvers::mixture_VLE_IO io;
    io.sstype = SaturationSolvers::imposed_p;
    io.Nstep_max = 2;
    
    // Set the pressure to a low pressure 
    HEOS._p = 100000;
    HEOS._Q = 1;
    
    // Get an extremely rough guess by interpolation of ln(p) v. T curve where the limits are mole-fraction-weighted
    long double Tguess = SaturationSolvers::saturation_preconditioner(HEOS, HEOS._p, SaturationSolvers::imposed_p, HEOS.mole_fractions);

    // Use Wilson iteration to obtain updated guess for temperature
    Tguess = SaturationSolvers::saturation_Wilson(HEOS, HEOS._Q, HEOS._p, SaturationSolvers::imposed_p, HEOS.mole_fractions, Tguess);
    
    // Actually call the successive substitution solver
    io.beta = 1;
    SaturationSolvers::successive_substitution(HEOS, HEOS._Q, Tguess, HEOS._p, HEOS.mole_fractions, HEOS.K, io);
        
    // Use the residual function based on x_i, T and rho' as independent variables.  rho'' is specified
    SaturationSolvers::newton_raphson_saturation NR;
    SaturationSolvers::newton_raphson_saturation_options IO;
    
    IO.bubble_point = false; // Do a "dewpoint" calculation all the way around
    IO.x = io.x;
    IO.y = HEOS.mole_fractions;
    IO.rhomolar_liq = io.rhomolar_liq;
    IO.rhomolar_vap = io.rhomolar_vap;
    IO.T = io.T;
    IO.p = io.p;
    IO.Nstep_max = 30;
    
    bool dont_extrapolate = false;
    
    PhaseEnvelopeData &env = HEOS.PhaseEnvelope;
    env.resize(HEOS.mole_fractions.size());

    std::size_t iter = 0, //< The iteration counter
                iter0 = 0; //< A reference point for the counter, can be increased to go back to linear interpolation
    long double factor = 1.05;
    for (;;)
    {
        top_of_loop: ; // A goto label so that nested loops can break out to the top of this loop
        
        if (failure_count > 5){
            // Stop since we are stuck at a bad point
            //throw SolutionError("stuck");
            return;
        }
        
        if (iter - iter0 > 0){ IO.rhomolar_vap *= factor;}
        long double x = IO.rhomolar_vap;
        if (dont_extrapolate)
        {
            // Reset the step to a reasonably small size
            factor = 1.0001;
        }
        else if (iter - iter0 == 2)
        {
            IO.T = LinearInterp(env.rhomolar_vap, env.T, iter-2, iter-1, x);
            IO.rhomolar_liq = LinearInterp(env.rhomolar_vap, env.rhomolar_liq, iter-2, iter-1, x);
            for (std::size_t i = 0; i < IO.x.size()-1; ++i) // First N-1 elements
            {            
                IO.x[i] = LinearInterp(env.rhomolar_vap, env.x[i], iter-2, iter-1, x);
            }
        }
        else if (iter - iter0 == 3)
        {
            IO.T = QuadInterp(env.rhomolar_vap, env.T, iter-3, iter-2, iter-1, x);
            IO.rhomolar_liq = QuadInterp(env.rhomolar_vap, env.rhomolar_liq, iter-3, iter-2, iter-1, x);
            for (std::size_t i = 0; i < IO.x.size()-1; ++i) // First N-1 elements
            {
                IO.x[i] = QuadInterp(env.rhomolar_vap, env.x[i], iter-3, iter-2, iter-1, x);
            }
        }
        else if (iter - iter0 > 3)
        {
            IO.T = CubicInterp(env.rhomolar_vap, env.T, iter-4, iter-3, iter-2, iter-1, x);
            IO.rhomolar_liq = CubicInterp(env.rhomolar_vap, env.rhomolar_liq, iter-4, iter-3, iter-2, iter-1, x);
            
            // Check if there is a large deviation from linear interpolation - this suggests a step size that is so large that a minima or maxima of the interpolation function is crossed
            long double T_linear = LinearInterp(env.rhomolar_vap, env.T, iter-2, iter-1, x);
            if (std::abs((T_linear-IO.T)/IO.T) > 0.1){
                // Try again, but with a smaller step
                IO.rhomolar_vap /= factor;
                factor = 1 + (factor-1)/2;
                failure_count++;
                continue;
            }
            for (std::size_t i = 0; i < IO.x.size()-1; ++i) // First N-1 elements
            {
                IO.x[i] = CubicInterp(env.rhomolar_vap, env.x[i], iter-4, iter-3, iter-2, iter-1, x);
                if (IO.x[i] < 0 || IO.x[i] > 1){
                    // Try again, but with a smaller step
                    IO.rhomolar_vap /= factor;
                    factor = 1 + (factor-1)/2;
                    failure_count++;
                    goto top_of_loop;
                }
            }
        }
    
        // The last mole fraction is sum of N-1 first elements
        IO.x[IO.x.size()-1] = 1 - std::accumulate(IO.x.begin(), IO.x.end()-1, 0.0);
        
        // Uncomment to check guess values for Newton-Raphson
        //std::cout << "\t\tdv " << IO.rhomolar_vap << " dl " << IO.rhomolar_liq << " T " << IO.T << " x " << vec_to_string(IO.x, "%0.10Lg") << std::endl;
        
        // Dewpoint calculation, liquid (x) is incipient phase
        try{
            NR.call(HEOS, IO.y, IO.x, IO);
        }
        catch(std::exception &e){
            std::cout << e.what() << std::endl;
            // Try again, but with a smaller step
            IO.rhomolar_vap /= factor;
            factor = 1 + (factor-1)/2;
            failure_count++;
            continue;
        }
        
        std::cout << "dv " << IO.rhomolar_vap << " dl " << IO.rhomolar_liq << " T " << IO.T << " p " << IO.p  << " hl " << IO.hmolar_liq  << " hv " << IO.hmolar_vap  << " sl " << IO.smolar_liq  << " sv " << IO.smolar_vap << " x " << vec_to_string(IO.x, "%0.10Lg")  << " Ns " << IO.Nsteps << std::endl;
        env.store_variables(IO.T, IO.p, IO.rhomolar_liq, IO.rhomolar_vap, IO.hmolar_liq, IO.hmolar_vap, IO.smolar_liq, IO.smolar_vap, IO.x, IO.y);
        
        iter ++;

        long double abs_rho_difference = std::abs((IO.rhomolar_liq - IO.rhomolar_vap)/IO.rhomolar_liq);
        
        // Critical point jump
        if (abs_rho_difference < 0.01 && IO.rhomolar_liq  > IO.rhomolar_vap){
            //std::cout << "dv" << IO.rhomolar_vap << " dl " << IO.rhomolar_liq << " " << vec_to_string(IO.x, "%0.10Lg") << " " << vec_to_string(IO.y, "%0.10Lg") << std::endl;
            long double rhoc_approx = 0.5*IO.rhomolar_liq + 0.5*IO.rhomolar_vap;
            long double rho_vap_new = 2*rhoc_approx - IO.rhomolar_vap;
            // Linearly interpolate to get new guess for T
            IO.T = LinearInterp(env.rhomolar_vap,env.T,iter-2,iter-1,rho_vap_new);
            IO.rhomolar_liq = 2*rhoc_approx - IO.rhomolar_liq;
            for (std::size_t i = 0; i < IO.x.size()-1; ++i){
                IO.x[i] = 2*IO.y[i] - IO.x[i];
            }
            IO.x[IO.x.size()-1] = 1 - std::accumulate(IO.x.begin(), IO.x.end()-1, 0.0);
            factor = rho_vap_new/IO.rhomolar_vap;
            dont_extrapolate = true; // So that we use the mole fractions we calculated here instead of the extrapolated values
            //std::cout << "dv " << rho_vap_new << " dl " << IO.rhomolar_liq << " " << vec_to_string(IO.x, "%0.10Lg") << " " << vec_to_string(IO.y, "%0.10Lg") << std::endl;
            iter0 = iter - 1; // Back to linear interpolation again
            continue;
        }
        
        dont_extrapolate = false;
        if (iter < 5){continue;}
        if (IO.Nsteps > 10)
        {
            factor = 1 + (factor-1)/10;
        }
        else if (IO.Nsteps > 5)
        {
            factor = 1 + (factor-1)/3;
        }
        else if (IO.Nsteps <= 4)
        {
            factor = 1 + (factor-1)*2;
        }
        // Min step is 1.01
        factor = std::max(factor, static_cast<long double>(1.01));
        
        // Stop if the pressure is below the starting pressure
        if (iter > 4 && IO.p < env.p[0]){ return; }
        
        // Reset the failure counter
        failure_count = 0;
    }
}

void PhaseEnvelopeRoutines::finalize(HelmholtzEOSMixtureBackend &HEOS)
{
    enum maxima_points {PMAX_SAT = 0, TMAX_SAT = 1};
    std::size_t imax; // Index of the maximal temperature or pressure
    
    PhaseEnvelopeData &env = HEOS.PhaseEnvelope;
    
    // Find the index of the point with the highest temperature
    std::size_t iTmax = std::distance(env.T.begin(), std::max_element(env.T.begin(), env.T.end()));
    
    // Find the index of the point with the highest pressure
    std::size_t ipmax = std::distance(env.p.begin(), std::max_element(env.p.begin(), env.p.end()));
        
    // Determine if the phase envelope corresponds to a Type I mixture
    // For now we consider a mixture to be Type I if the pressure at the 
    // end of the envelope is lower than max pressure pressure
    env.TypeI = env.p[env.p.size()-1] < env.p[ipmax];
    
    // Approximate solutions for the maxima of the phase envelope
    // See method in Gernert.  We use our spline class to find the coefficients
    if (env.TypeI){
        for (int imaxima = 0; imaxima <= 1; ++imaxima){
            maxima_points maxima;
            if (imaxima == PMAX_SAT){
                maxima = PMAX_SAT;
            }
            else if (imaxima == TMAX_SAT){
                maxima = TMAX_SAT;
            }
            
            // Spline using the points around it
            SplineClass spline;
            if (maxima == TMAX_SAT){
                imax = iTmax;
                spline.add_4value_constraints(env.rhomolar_vap[iTmax-1], env.rhomolar_vap[iTmax], env.rhomolar_vap[iTmax+1], env.rhomolar_vap[iTmax+2],
                                              env.T[iTmax-1], env.T[iTmax], env.T[iTmax+1], env.T[iTmax+2] );
            }
            else{
                imax = ipmax;
                spline.add_4value_constraints(env.rhomolar_vap[ipmax-1], env.rhomolar_vap[ipmax], env.rhomolar_vap[ipmax+1], env.rhomolar_vap[ipmax+2],
                                              env.p[ipmax-1], env.p[ipmax], env.p[ipmax+1], env.p[ipmax+2] );
            }
            spline.build(); // y = a*rho^3 + b*rho^2 + c*rho + d
            
            // Take derivative
            // dy/drho = 3*a*rho^2 + 2*b*rho + c
            // Solve quadratic for derivative to find rho
            int Nsoln = 0; double rho0 = _HUGE, rho1 = _HUGE, rho2 = _HUGE;
            solve_cubic(0, 3*spline.a, 2*spline.b, spline.c, Nsoln, rho0, rho1, rho2);
            
            SaturationSolvers::newton_raphson_saturation_options IO;
            IO.rhomolar_vap = _HUGE;
            // Find the correct solution
            if (Nsoln == 1){
                IO.rhomolar_vap = rho0;
            }
            else if (Nsoln == 2){
                if (is_in_closed_range(env.rhomolar_vap[imax-1], env.rhomolar_vap[imax+1], (long double)rho0)){ IO.rhomolar_vap = rho0; }
                if (is_in_closed_range(env.rhomolar_vap[imax-1], env.rhomolar_vap[imax+1], (long double)rho1)){ IO.rhomolar_vap = rho1; }
            }
            else{
                throw ValueError("More than 2 solutions found");
            }
            
            class solver_resid : public FuncWrapper1D
            {
            public:
                std::size_t imax;
                maxima_points maxima;
                HelmholtzEOSMixtureBackend *HEOS;
                SaturationSolvers::newton_raphson_saturation NR;
                SaturationSolvers::newton_raphson_saturation_options IO;
                solver_resid(HelmholtzEOSMixtureBackend &HEOS, std::size_t imax, maxima_points maxima)
                {
                    this->HEOS = &HEOS, this->imax = imax; this->maxima = maxima;
                };
                double call(double rhomolar_vap){
                    PhaseEnvelopeData &env = HEOS->PhaseEnvelope;
                    IO.bubble_point = false;
                    IO.rhomolar_vap = rhomolar_vap;
                    IO.y = HEOS->get_mole_fractions();
                    IO.x = IO.y; // Just to give it good size
                    IO.T = CubicInterp(env.rhomolar_vap, env.T, imax-1, imax, imax+1, imax+2, IO.rhomolar_vap);
                    IO.rhomolar_liq = CubicInterp(env.rhomolar_vap, env.rhomolar_liq, imax-1, imax, imax+1, imax+2, IO.rhomolar_vap);
                    for (std::size_t i = 0; i < IO.x.size()-1; ++i) // First N-1 elements
                    {
                        IO.x[i] = CubicInterp(env.rhomolar_vap, env.x[i], imax-1, imax, imax+1, imax+2, IO.rhomolar_vap);
                    }
                    IO.x[IO.x.size()-1] = 1 - std::accumulate(IO.x.begin(), IO.x.end()-1, 0.0);
                    NR.call(*HEOS, IO.y, IO.x, IO);
                    if (maxima == TMAX_SAT){
                        return NR.dTsat_dPsat;
                    }
                    else{
                        return NR.dPsat_dTsat;
                    }
                };
            };
            
            solver_resid resid(HEOS, imax, maxima);
            std::string errstr;
            double rho = Brent(resid, IO.rhomolar_vap*0.95, IO.rhomolar_vap*1.05, DBL_EPSILON, 1e-12, 100, errstr);
            
            // If maxima point is greater than density at point from the phase envelope, increase index by 1 so that the 
            // insertion will happen *after* the point in the envelope since density is monotonically increasing.
            if (rho > env.rhomolar_vap[imax]){ imax++; }
            
            env.insert_variables(resid.IO.T, resid.IO.p, resid.IO.rhomolar_liq, resid.IO.rhomolar_vap, resid.IO.hmolar_liq, 
                                 resid.IO.hmolar_vap, resid.IO.smolar_liq, resid.IO.smolar_vap, resid.IO.x, resid.IO.y, imax);
            
            if (maxima == TMAX_SAT){
                env.iTsat_max = imax;
                if (imax <= env.ipsat_max){
                    // Psat_max goes first; an insertion at a smaller index bumps up the index 
                    // for ipsat_max, so we bump the index to keep pace
                    env.ipsat_max++;
                }
            }
            else{
                env.ipsat_max = imax;
            }
        }
    }
}

} /* namespace CoolProp */

#endif