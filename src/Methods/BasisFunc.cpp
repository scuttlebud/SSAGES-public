/**
 * This file is part of
 * SSAGES - Suite for Advanced Generalized Ensemble Simulations
 *
 * Copyright 2016 Joshua Moller <jmoller@uchicago.edu>
 *           2017 Julian Helfferich <julian.helfferich@gmail.com>
 *
 * SSAGES is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SSAGES is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SSAGES.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "BasisFunc.h"
#include <cmath>
#include <iostream>
#include <iomanip>

namespace mpi = boost::mpi;
namespace SSAGES
{

	// Pre-simulation hook.
	void Basis::PreSimulation(Snapshot* snapshot, const CVList& cvs)
	{
        // For print statements and file I/O, the walker IDs are used
        mpiid_ = snapshot->GetWalkerID();

        // Make sure the iteration index is set correctly
        iteration_ = 0;

        // There are a few error messages / checks that are in place with
        // defining CVs and grids
        if(hist_->GetDimension() != cvs.size())
        {
            std::cerr<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
            std::cerr<<"ERROR: Histogram dimensions doesn't match number of CVS."<<std::endl;
            std::cerr<<"Exiting on node ["<<mpiid_<<"]"<<std::endl;
            std::cerr<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
            world_.abort(EXIT_FAILURE);
        }
        else if(cvs.size() != polyords_.size())
        {
            std::cout<<cvs.size()<<std::endl;
            std::cout<<polyords_.size()<<std::endl;
            std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
            std::cout<<"WARNING: The number of polynomial orders is not the same"<<std::endl;
            std::cout<<"as the number of CVs"<<std::endl;
            std::cout<<"The simulation will take the first defined input"<<std::endl;
            std::cout<<"as the same for all CVs. ["<<polyords_[0]<<"]"<<std::endl;
            std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;

            //! Resize the polynomial vector so that it doesn't crash here
            polyords_.resize(cvs.size());
            //! And now reinitialize the vector
            for(size_t i = 0; i < cvs.size(); ++i)
            {
                polyords_[i] = polyords_[0];
            }
        }

        // This is to check for non-periodic bounds. It comes into play in the update bias function
        bounds_ = true;
                 
        size_t coeff_size = 1;

        for(size_t i = 0; i < cvs.size(); ++i)
        {
            coeff_size *= polyords_[i]+1;
        }

		derivatives_.resize(cvs.size());
        unbias_.resize(hist_->size(),0);
        coeff_arr_.resize(coeff_size,0);

        std::vector<int> idx(cvs.size(), 0);
        std::vector<int> jdx(cvs.size(), 0);
		Map temp_map(idx,0.0);
        
        // Initialize the mapping for the hist function
        for (int &val : *hist_) {
            val = 0;
        }

        //Initialize the mapping for the coeff function
        for(size_t i = 0; i < coeff_size; ++i)
        {
            for(size_t j = 0; j < jdx.size(); ++j)
            {
                if(jdx[j] > 0 && jdx[j] % (polyords_[j]+1) == 0)
                {
                    if(j != cvs.size() - 1)
                    { 
                        jdx[j+1]++;
                        jdx[j] = 0;
                    }
                }
                temp_map.map[j] = jdx[j];
				temp_map.value  = 0; 
            }
			coeff_.push_back(temp_map);           
            coeff_[i].value = coeff_arr_[i];
            jdx[0]++;
        }

        //Initialize the look-up table.
        BasisInit(cvs);
	}

	// Post-integration hook.
	void Basis::PostIntegration(Snapshot* snapshot, const CVList& cvs)
	{
        std::vector<double> x(cvs.size(),0);

        /*The binned cv space is updated at every step
         *After a certain number of steps has been passed, the system updates a
         *bias projection based on the visited histogram states
         */
        for(size_t i = 0; i < cvs.size(); ++i)
        {
            x[i] = cvs[i]->GetValue();
        }
       
        // The histogram is updated based on the index
        hist_->at(x) += 1;
    
        // Update the basis projection after a predefined number of steps
        if(snapshot->GetIteration()  % cyclefreq_ == 0) {
            double beta;
            beta = 1.0 / (snapshot->GetTemperature() * snapshot->GetKb());

            // For systems with poorly defined temperature (ie: 1 particle) the
            // user needs to define their own temperature. This is a hack that
            // will be removed in future versions.

            if(snapshot->GetTemperature() == 0)
            {
                beta = temperature_;
                if(temperature_ == 0)
                {
                    std::cout<<std::endl;
                    std::cerr<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    std::cerr<<"ERROR: Input temperature needs to be defined for this simulation"<<std::endl;
                    std::cerr<<"Exiting on node ["<<mpiid_<<"]"<<std::endl;
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    exit(EXIT_FAILURE);
                }
            }
            iteration_+= 1;
            UpdateBias(cvs,beta);
            std::cout<<"Node: ["<<mpiid_<<"]"<<std::setw(10)<<"\tSweep: "<<iteration_<<std::endl;
        }

		// This calculates the bias force based on the existing basis projection.
		CalcBiasForce(cvs);

		// Take each CV and add its biased forces to the atoms using the chain rule
		auto& forces = snapshot->GetForces();
        auto& virial = snapshot->GetVirial();
		for(size_t i = 0; i < cvs.size(); ++i)
		{
			auto& grad = cvs[i]->GetGradient();
            auto& boxgrad = cvs[i]->GetBoxGradient();

			/* Update the forces in snapshot by adding in the force bias from each
			 *CV to each atom based on the gradient of the CV.
             */
			for (size_t j = 0; j < forces.size(); ++j) 
				forces[j] += derivatives_[i]*grad[j];
            
            virial -= derivatives_[i]*boxgrad;
		}
	}

	// Post-simulation hook.
	void Basis::PostSimulation(Snapshot*, const CVList&)
	{
	    std::cout<<"Run has finished"<<std::endl;	
	}

    /* The basis set is initialized through the recursive definition.
     *Currently, SSAGES only supports Legendre polyonmials for basis projections
     */
    void Basis::BasisInit(const CVList& cvs)
    {
		for( size_t k = 0; k < cvs.size(); k++)
		{
			size_t ncoeff = polyords_[k]+1;
            int nbins = hist_->GetNumPoints(k);

            std::vector<double> dervs(nbins*ncoeff,0);
            std::vector<double> vals(nbins*ncoeff,0);
            std::vector<double> x(nbins,0);

            /*As the values for Legendre polynomials can be defined recursively, \
             *both the derivatives and values are defined at the same time,
             */
			for (int i = 0; i < nbins; ++i)
			{
                x[i] = (2.0*i + 1.0)/nbins - 1.0;
				vals[i] = 1.0;
				dervs[i] = 0.0;
			}

			for (int i = 0; i < nbins; ++i)
			{
				vals[i+nbins] = x[i];
				dervs[i+nbins] = 1.0;
			}

			for (size_t j = 2; j < ncoeff; j++)
			{
				for (int i = 0; i < nbins; i++)
				{
                    //Evaluate the values of the Legendre polynomial at each bin
					vals[i+j*nbins] = ( ( 2.0*j - 1.0 ) * x[i] * vals[i+(j-1)*nbins]
					- (j - 1.0) * vals[i+(j-2)*nbins] ) / j;

                    //Evaluate the derivatives of the Legendre polynomial at each bin
                    dervs[i+j*nbins] = ( ( 2.0*j - 1.0 ) * ( vals[i+(j-1)*nbins] + x[i] * dervs[i+(j-1)*nbins] )
                    - (j - 1.0) * dervs[i+(j-2)*nbins] ) / j;
				}
			}
            BasisLUT TempLUT(vals,dervs);
            LUT_.push_back(TempLUT);
        }
	}
    
	// Update the coefficients/bias projection
	void Basis::UpdateBias(const CVList& cvs, const double beta)
	{
        std::vector<double> x(cvs.size(), 0);
        std::vector<double> coeffTemp(coeff_.size(), 0);
        double sum  = 0.0;
        double bias = 0.0;
        double basis = 1.0;

        // For multiple walkers, the struct is unpacked
        Histogram<int> histlocal(*hist_);

        // Summed between all walkers
        MPI_Allreduce(histlocal.data(), hist_->data(), hist_->size(), MPI_INT, MPI_SUM, world_);

        // Construct the biased histogram
        size_t i = 0;
        for (Histogram<int>::iterator it2 = hist_->begin(); it2 != hist_->end(); ++it2, ++i)
        {
            if (it2.isUnderOverflowBin()) {
                --i;
                continue;
            }

            // This is to make sure that the CV projects across the entire surface
            if (*it2 == 0) { *it2 = 1; }
           
            // The loop builds the previous basis projection for each bin of the histogram
            for(size_t k = 1; k < coeff_.size(); ++k)
            {
                auto& coeff = coeff_[k];
                for(size_t l = 0; l < cvs.size(); ++l)
                { 
                    // The previous bias is only calculated after each sweep has happened
                    int nbins = hist_->GetNumPoints(l);
                    basis *= LUT_[l].values[it2.index(l) + coeff.map[l]*nbins];
                }
                bias += coeff.value*basis;
                basis = 1.0;
            }
            
            /* The evaluation of the biased histogram which projects the histogram to the
             * current bias of CV space.
             */
            unbias_[i] += (*it2) * exp(bias) * weight_ / (double)(cyclefreq_);
            bias = 0.0;

        }

        // The coefficients and histograms are reset after evaluating the biased histogram values
        for(size_t i = 0; i < coeff_.size(); ++i)
        {
            coeffTemp[i] = coeff_[i].value;
            coeff_[i].value = 0.0;
        }

        // Reset histogram
        for (int &val : *hist_) {
            val = 0;
        }

        // The loop that evaluates the new coefficients by integrating the CV space
        for(size_t i = 1; i < coeff_.size(); ++i)
        {
            auto& coeff = coeff_[i];
            
            // The method uses a standard integration with trap rule weights
            size_t j = 0;
            for(Histogram<int>::iterator it2 = hist_->begin(); it2 != hist_->end(); ++it2, ++j)
            {
                if (it2.isUnderOverflowBin()) {
                    --j;
                    continue;
                }

                double weight = std::pow(2.0,cvs.size());

                // This adds in a trap-rule type weighting which lowers error significantly at the boundaries
                for(size_t k = 0; k < cvs.size(); ++k)
                {
                    if( it2.index(k) == 0 ||
                        it2.index(k) == hist_->GetNumPoints(k)-1)
                        weight /= 2.0;
                }
            
                /*The numerical integration of the biased histogram across the entirety of CV space
                 *All calculations include the normalization as well
                 */
                for(size_t l = 0; l < cvs.size(); l++)
                {
                    int nbins = hist_->GetNumPoints(l);
                    basis *= LUT_[l].values[it2.index(l) + coeff.map[l]*nbins] / nbins;
                    basis *= 2.0 * coeff.map[l] + 1.0;
                }
                coeff.value += basis * log(unbias_[j]) * weight/std::pow(2.0,cvs.size());
                basis = 1.0;
            }
            coeffTemp[i] -= coeff.value;
            coeff_arr_[i] = coeff.value;
            sum += coeffTemp[i]*coeffTemp[i];
        }

        if(world_.rank() == 0)
            // Write coeff at this step, but only one walker
            PrintBias(cvs,beta);

        // The convergence tolerance and whether the user wants to exit are incorporated here
        if(sum < tol_)
        {
            std::cout<<"System has converged"<<std::endl;
            if(converge_exit_)
            {
                std::cout<<"User has elected to exit. System is now exiting"<<std::endl;
                exit(EXIT_SUCCESS);
            }
        }
	}

    /*The coefficients are printed out for the purpose of saving the free energy space
     *Additionally, the current basis projection is printed so that the user can view
     *the current free energy space
     */
    void Basis::PrintBias(const CVList& cvs, const double beta)
    {
        std::vector<double> bias(hist_->size(), 0);
        std::vector<double> x(cvs.size(), 0);
        double temp = 1.0; 

        /* Since the coefficients are the only piece that needs to be
         *updated, the bias is only evaluated when printing
         */
        size_t i = 0;
        for(Histogram<int>::iterator it = hist_->begin(); it != hist_->end(); ++it, ++i)
        {
            if (it.isUnderOverflowBin()) {
                --i;
                continue;
            }

            for(size_t j = 1; j < coeff_.size(); ++j)
            {
                for(size_t k = 0; k < cvs.size(); ++k)
                {
                    int nbins = hist_->GetNumPoints(k);
                    temp *=  LUT_[k].values[it.index(k) + coeff_[j].map[k] * nbins];
                }
                bias[i] += coeff_[j].value*temp;
                temp  = 1.0;
            }
        }

        // The filenames will have a standard name, with a user-defined suffix
        std::string filename1 = "basis"+bnme_+".out";
        std::string filename2 = "coeff"+cnme_+".out";
    
		basisout_.precision(5);
        coeffout_.precision(5);
        basisout_.open(filename1.c_str());
        coeffout_.open(filename2.c_str());

        // The CV values, PMF projection, PMF, and biased histogram are output for the user
        coeffout_ << iteration_  <<std::endl;
        basisout_ << "CV Values" << std::setw(35*cvs.size()) << "Basis Set Bias" << std::setw(35) << "PMF Estimate" << std::setw(35) << "Biased Histogram" << std::endl;
        
        size_t j = 0;
        for(Histogram<int>::iterator it = hist_->begin(); it != hist_->end(); ++it, ++j)
        {
            if (it.isUnderOverflowBin()) {
                --j;
                continue;
            }

            for(size_t k = 0; k < cvs.size(); ++k)
            {
                // Evaluate the CV values for printing purposes
                basisout_ << it.coordinate(k) << std::setw(35);
            }
            basisout_ << -bias[j] << std::setw(35);
            if(unbias_[j])
                basisout_ << -log(unbias_[j]) / beta << std::setw(35);
            else
                basisout_ << "0" << std::setw(35);
            basisout_ << unbias_[j];
            basisout_ << std::endl;
        }

        for(size_t k = 0; k < coeff_.size(); ++k)
        {
            coeffout_ <<coeff_[k].value << std::endl;
        }

		basisout_ << std::endl;
        basisout_.close();
        coeffout_.close();
	}

    // The forces are calculated by chain rule, first  the derivatives of the basis set, then in the PostIntegration function, the derivative of the CV is evaluated
	void Basis::CalcBiasForce(const CVList& cvs)
	{	
		// Reset derivatives
        std::fill(derivatives_.begin(), derivatives_.end(), 0);
        std::vector<double> x(cvs.size(),0);

        double temp = 1.0;

        //This is calculating the derivatives for the bias force
        for (size_t j = 0; j < cvs.size(); ++j)
        {
            x[j] = cvs[j]->GetValue();
            double min = hist_->GetLower(j);
            double max = hist_->GetUpper(j);

            if(!hist_->GetPeriodic(j))
            {
                // In order to prevent the index for the histogram from going out of bounds a check is in place
                if(x[j] > max && bounds_)
                {
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    std::cout<<"WARNING: CV is above the maximum boundary."<<std::endl;
                    std::cout<<"Statistics will not be gathered during this interval"<<std::endl;
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    bounds_ = false;
                }
                else if(x[j] < min && bounds_)
                {
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    std::cout<<"WARNING: CV is below the minimum boundary."<<std::endl;
                    std::cout<<"Statistics will not be gathered during this interval"<<std::endl;
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    bounds_ = false;
                }
                else if(x[j] < max && x[j] > min && !bounds_)
                {
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    std::cout<<"CV has returned in between bounds. Run is resuming"<<std::endl;
                    std::cout<<"::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::"<<std::endl;
                    bounds_ = true;
                }
            }
        }

        // Only apply soft wall potential in the event that it has left the boundaries
        if(bounds_)
        {
            for (size_t i = 1; i < coeff_.size(); ++i)
            {
                for (size_t j = 0; j < cvs.size(); ++j)
                {
                    temp = 1.0;
                    for (size_t k = 0; k < cvs.size(); ++k)
                    {
                        int nbins = hist_->GetNumPoints(k);
                        temp *= j == k ?  LUT_[k].derivs[hist_->GetIndices(x)[k] + coeff_[i].map[k]*nbins] * 2.0 / (hist_->GetUpper(j) - hist_->GetLower(j))
                                       :  LUT_[k].values[hist_->GetIndices(x)[k] + coeff_[i].map[k]*nbins];
                    }
                    derivatives_[j] -= coeff_[i].value * temp;
                }
            }
        }
        
        // This is where the wall potentials are going to be thrown into the method if the system is not a periodic CV
        for(size_t j = 0; j < cvs.size(); ++j)
        {
            // Are these used?
            // double min = hist_->GetLower(j);
            // double max = hist_->GetUpper(j);

            if(!hist_->GetPeriodic(j))
            {
                if(x[j] > boundUp_[j])
                    derivatives_[j] -= restraint_[j] * (x[j] - boundUp_[j]);
                else if(x[j] < boundLow_[j])
                    derivatives_[j] -= restraint_[j] * (x[j] - boundLow_[j]);
            }
        }
    }
}
