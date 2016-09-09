/**
 * This file is part of
 * SSAGES - Suite for Advanced Generalized Ensemble Simulations
 *
 * Copyright 2016 Yamil Colon <yamilcolon2015@u.northwestern.edu>
 *                Hythem Sidky <hsidky@nd.edu>
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

#pragma once 

#include "Drivers/DriverException.h"
#include "CollectiveVariable.h"

#include <array>
#include <cmath>

namespace SSAGES
{
	// Collective variable to calculate angle.
	class AngleCV : public CollectiveVariable
	{
	private:
		// Vector of 3 atom ID's of interest.
		Label _atomids;

	public:

		//! Constructor.
		/*!
		 * \param atomid1 ID of the first atom defining the angle.
		 * \param atomid2 ID of the second atom defining the angle.
		 * \param atomid3 ID of the third atom defining the angle.
		 * \param periodic If \c True consider periodic boundary conditions.
		 *
		 * Construct an dihedral CV.
		 *
		 * \todo Bounds needs to be an input and periodic boundary conditions
		 */
		AngleCV(int atomid1, int atomid2, int atomid3) :
		_atomids({atomid1, atomid2, atomid3})
		{
			_bounds = {{0,M_PI}};
		}

		//! Initialize necessary variables.
		/*!
		 * \param snapshot Current simulation snapshot.
		 */
		void Initialize(const Snapshot& snapshot) override
		{
			using std::to_string;
			
			std::vector<int> found;
			snapshot.GetLocalIndices(_atomids, &found);
			int nfound = found.size();
			MPI_Allreduce(MPI_IN_PLACE, &nfound, 1, MPI_INT, MPI_SUM, snapshot.GetCommunicator());
			
			if(nfound != 3)
				throw BuildException({
					"TorsionalCV: Expected to find " + 
					to_string(3) + 
					" atoms, but only found " + 
					to_string(nfound) + "."
				});	
		}

		//! Evaluate the CV.
		/*!
		 * \param snapshot Current simulation snapshot.
		 */
		void Evaluate(const Snapshot& snapshot) override
		{
			// Get data from snapshot. 
			auto n = snapshot.GetNumAtoms();
			const auto& pos = snapshot.GetPositions();
			auto& comm = snapshot.GetCommunicator();

			// Initialize gradient.
			_grad.resize(n, Vector3{0,0,0});

			Vector3 xi{0, 0, 0}, xj{0, 0, 0}, xk{0, 0, 0};

			auto iindex = snapshot.GetLocalIndex(_atomids[0]);
			auto jindex = snapshot.GetLocalIndex(_atomids[1]);
			auto kindex = snapshot.GetLocalIndex(_atomids[2]);

			if(iindex != -1) xi = pos[iindex];
			if(jindex != -1) xj = pos[jindex];
			if(kindex != -1) xk = pos[kindex];

			// By performing a reduce, we actually collect all. This can 
			// be converted to a more intelligent allgater on rank then bcast. 
			MPI_Allreduce(MPI_IN_PLACE, xi.data(), 3, MPI_DOUBLE, MPI_SUM, comm);
			MPI_Allreduce(MPI_IN_PLACE, xj.data(), 3, MPI_DOUBLE, MPI_SUM, comm);
			MPI_Allreduce(MPI_IN_PLACE, xk.data(), 3, MPI_DOUBLE, MPI_SUM, comm);

			// Two vectors
			auto rij = snapshot.ApplyMinimumImage(xi - xj);
			auto rkj = snapshot.ApplyMinimumImage(xk - xj);

			auto dotP = rij.dot(rkj);
			auto nrij = rij.norm();
			auto nrkj = rkj.norm();

			_val = acos(dotP/(nrij*nrkj));

			// Calculate gradients
			auto prefactor = -1.0/(sqrt(1. - dotP/(nrij*nrkj))*nrij*nrkj);

			if(iindex != -1) _grad[iindex] = prefactor*(rkj - dotP*rij/(nrij*nrij));	
			if(kindex != -1) _grad[kindex] = prefactor*(rij - dotP*rkj/(nrkj*nrkj));
			MPI_Allreduce(MPI_IN_PLACE, _grad[iindex].data(), 3, MPI_DOUBLE, MPI_SUM, comm);
			MPI_Allreduce(MPI_IN_PLACE, _grad[kindex].data(), 3, MPI_DOUBLE, MPI_SUM, comm);
			if(jindex != -1) _grad[jindex] = -_grad[iindex] - _grad[kindex];
		}

		//! Serialize this CV for restart purposes.
		/*!
		 * \param json JSON value
		 */
		void Serialize(Json::Value& json) const override
		{
			json["type"] = "Angle";

			for(auto& id : _atomids)
				json["atom_ids"].append(id);

			for(auto& bound : _bounds)
				json["bounds"].append(bound);
		}

	};
}
