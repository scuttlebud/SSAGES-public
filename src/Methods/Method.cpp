#include "Method.h"
#include "json/json.h"
#include "schema.h"
#include "../Validator/ObjectRequirement.h"
#include "../Validator/ArrayRequirement.h"
#include "ElasticBand.h"
#include "FiniteTempString.h"
#include "Meta.h"
#include "Umbrella.h"

using namespace Json;

namespace SSAGES
{
	Method* Method::BuildMethod(const Json::Value &json, 
						boost::mpi::communicator& world, 
						boost::mpi::communicator& comm)
	{
		return BuildMove(json, world, comm, "#/methods");
	}

	Method* Method::BuildMethod(const Value &json, 
						boost::mpi::communicator& world, 
						boost::mpi::communicator& comm,
						const std::string& path)
	{
		ObjectRequirement validator;
		Value schema;
		Reader reader;

		Method* method = nullptr;

		// Random device for seed generation. 
		// std::random_device rd;
		// auto maxi = std::numeric_limits<int>::max();
		// auto seed = json.get("seed", rd() % maxi).asUInt();

		// Get move type. 
		std::string type = json.get("type", "none").asString();

		if(type == "Umbrella")
		{
			reader.parse(JsonSchema::UmbrellaMethod, schema);
			validator.Parse(schema, path);

			// Validate inputs.
			validator.Validate(json, path);
			if(validator.HasErrors())
				throw BuildException(validator.GetErrors());

			std::vector<std::double> ksprings;
			for(auto& s : json["ksprings"])
				ksprings.push_back(s.asDouble());

			std::vector<std::double> centers;
			for(auto& s : json["centers"])
				centers.push_back(s.asDouble());

			if(ksprings.size() != centers.size())
			{
				std::cout<<"Need to define a spring fro every center or 
					a center for every spring!"<<std::endl;

				exit(0);
			}

			auto freq = json.get("frequency", 1).asInt();

			auto* m = new Umbrella(world, comm, ksprings, centers, freq);

			method = static_cast<Method*>(m);
		}
		else if(type == "Metadynamics")
		{
			reader.parse(JsonSchema::MetadynamicsMethod, schema);
			validator.Parse(schema, path);

			// Validate inputs.
			validator.Validate(json, path);
			if(validator.HasErrors())
				throw BuildException(validator.GetErrors());

			std::vector<std::double> widths;
			for(auto& s : json["widths"])
				widths.push_back(s.asDouble());

			auto height = json.get("height", 1.0).asDouble();
			auto hillfreq = json.get("hill frequency", 1).asInt();
			auto freq = json.get("frequency", 1).asInt();			

			auto* m = new Meta(world, comm, height, widths, hillfreq, freq);

			method = static_cast<Method*>(m);
		}
		else if(type == "ElasticBand")
		{
			reader.parse(JsonSchema::ElasticBandMethod, schema);
			validator.Parse(schema, path);

			// Validate inputs.
			validator.Validate(json, path);
			if(validator.HasErrors())
				throw BuildException(validator.GetErrors());

			std::vector<std::double> ksprings;
			for(auto& s : json["ksprings"])
				ksprings.push_back(s.asDouble());

			std::vector<std::double> centers;
			for(auto& s : json["centers"])
				centers.push_back(s.asDouble());

			auto isteps = json.get("max iterations", 2000).asInt();
			auto eqsteps = json.get("equilibration steps", 20).asInt();
			auto evsteps = json.get("evolution steps", 20).asInt();
			auto nsamples = json.get("number samples", 20).asInt();
			auto stringspring = json.get("kstring", 10.0).asDouble();
			auto timestep = json.get("time step", 1.0).asDouble();			
			auto freq = json.get("frequency", 1).asInt();			

			auto* m = new ElasticBand(world, comm, isteps, eqsteps,
			 						evsteps, nsamples, centers, ksprings,
			 						stringspring, timestep, frequency);

			method = static_cast<Method*>(m);
		}
		else if(type == "FiniteTemperatureString")
		{
			reader.parse(JsonSchema::FiniteTemperatureMethod, schema);
			validator.Parse(schema, path);

			// Validate inputs.
			validator.Validate(json, path);
			if(validator.HasErrors())
				throw BuildException(validator.GetErrors());

			std::vector<std::double> centers;
			for(auto& s : json["centers"])
				centers.push_back(s.asDouble());

			auto isteps = json.get("block iterations", 2000).asInt();
			auto nsamples = json.get("number samples", 20).asInt();
			auto stringspring = json.get("kappa", 0.1).asDouble();
			auto timestep = json.get("time step", 0.1).asDouble();			
			auto freq = json.get("frequency", 1).asInt();			

			NumNodes = comm.size();
			auto* m = new ElasticBand(world, comm, isteps, 
									centers, NumNodes, kappa,
			 						tau, frequency);

			method = static_cast<Method*>(m);
		}
		else
		{
			throw BuildException({path + ": Unknown method type specified."});
		}

		return method;
	}
}