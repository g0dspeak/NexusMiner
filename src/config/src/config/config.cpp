
#include "config/config.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace nexusminer
{
namespace config
{
	Config::Config(std::shared_ptr<spdlog::logger> logger)
		: m_logger{std::move(logger)}
		, m_wallet_ip{ "127.0.0.1" }
		, m_port{ 9323 }
		, m_local_ip{"127.0.0.1"}
		, m_mining_mode{ Mining_mode::HASH}
		, m_use_pool{false}
		, m_pool_config{}
		, m_logfile{""}		// no logfile usage, default
		, m_connection_retry_interval{5}
		, m_print_statistics_interval{5}
		, m_get_height_interval{2}
	{
	}

	bool Config::read_config(std::string const& miner_config_file)
	{
		m_logger->info("Reading config file {}", miner_config_file);

		std::ifstream config_file(miner_config_file);
		if (!config_file.is_open())
		{
			m_logger->critical("Unable to read {}", miner_config_file);
			return false;
		}

		json j = json::parse(config_file);
		j.at("wallet_ip").get_to(m_wallet_ip);
		j.at("port").get_to(m_port);
		if (j.count("local_ip") != 0)
		{
			j.at("local_ip").get_to(m_local_ip);
		}

		std::string mining_mode = j["mining_mode"];
		std::for_each(mining_mode.begin(), mining_mode.end(), [](char & c) {
        	c = ::tolower(c);
    	});

		if(mining_mode == "prime")
		{
			m_mining_mode = Mining_mode::PRIME;
		}
		else
		{
			m_mining_mode = Mining_mode::HASH;
		}

		j.at("use_pool").get_to(m_use_pool);
		if(m_use_pool)
		{
			m_pool_config.m_username = j.at("pool")["username"];
		}

		// read stats printer config
		if(!read_stats_printer_config(j))
		{
			return false;
		}

		// read worker config
		if(!read_worker_config(j))
		{
			return false;
		}

		// advanced configs
		if (j.count("connection_retry_interval") != 0)
		{
			j.at("connection_retry_interval").get_to(m_connection_retry_interval);
		}
		if (j.count("print_statistics_interval") != 0)
		{
			j.at("print_statistics_interval").get_to(m_print_statistics_interval);
		}
		if (j.count("get_height_interval") != 0)
		{
			j.at("get_height_interval").get_to(m_get_height_interval);
		}

		j.at("logfile").get_to(m_logfile);
		print_worker_config();
		// TODO Need to add exception handling here and set return value appropriately
		return true;
	}

	bool Config::read_stats_printer_config(nlohmann::json& j)
	{
		for (auto& stats_printers_json : j["stats_printers"])
		{
			for(auto& stats_printer_config_json : stats_printers_json)
			{
				Stats_printer_config stats_printer_config;
				auto stats_printer_mode = stats_printer_config_json["mode"];

				if(stats_printer_mode == "console")
				{
					stats_printer_config.m_mode = Stats_printer_mode::CONSOLE;
					stats_printer_config.m_printer_mode = Stats_printer_config_console{};
				}
				else if(stats_printer_mode == "file")
				{
					stats_printer_config.m_mode = Stats_printer_mode::FILE;
					stats_printer_config.m_printer_mode = Stats_printer_config_file{stats_printer_config_json["filename"]};
				}
				else
				{
					// invalid config
					return false;
				}

				m_stats_printer_config.push_back(stats_printer_config);		
			}
		}
		return true;	
	}

	bool Config::read_worker_config(nlohmann::json& j)
	{
		for (auto& workers_json : j["workers"])
		{
			for(auto& worker_config_json : workers_json)
			{
				Worker_config worker_config;
				worker_config.m_id = worker_config_json["id"];

				auto& worker_mode_json = worker_config_json["mode"];

				if(worker_mode_json["hardware"] == "cpu")
				{
					worker_config.m_mode = Worker_mode::CPU;
					worker_config.m_worker_mode = Worker_config_cpu{};
				}
				else if(worker_mode_json["hardware"] == "gpu")
				{
					worker_config.m_mode = Worker_mode::GPU;
					worker_config.m_worker_mode = Worker_config_gpu{};
				}
				else if(worker_mode_json["hardware"] == "fpga")
				{
					worker_config.m_mode = Worker_mode::FPGA;
					worker_config.m_worker_mode = Worker_config_fpga{worker_mode_json["serial_port"]};
				}
				else
				{
					// invalid config
					return false;
				}

				m_worker_config.push_back(worker_config);		
			}
		}
		return true;	
	}

	void Config::print_worker_config() const
	{
		std::stringstream ss;
		ss << m_worker_config.size() << " workers configured" << std::endl;
		for (auto const& worker : m_worker_config)
		{
			std::string mode{};
			switch (worker.m_mode)
			{
			case Worker_mode::CPU: mode = "CPU"; break;
			case Worker_mode::GPU: mode = "GPU"; break;
			case Worker_mode::FPGA: mode = "FPGA"; break;
			}
			ss << worker.m_id << " mode: " << mode << std::endl;
		}

		m_logger->info(ss.str());
	}
}
}