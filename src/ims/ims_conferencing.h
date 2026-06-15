#pragma once
#include <vector>
#include <string>
#include <map>
#include "common/logger.h"

/**
 * ENGINEERING LEVEL: IMS Conferencing (AS/MRF)
 * In real VoLTE, the S-CSCF triggers the Application Server (TAS) 
 * which then uses a Media Resource Function (MRF) to mix audio.
 */
class ConferenceBridge {
public:
    void createConference(uint32_t conf_id, const std::string& initiator) {
        participants_[conf_id].push_back(initiator);
        Logger::pcscf(Logger::Level::ENGINEER, "Created Conf ID: " + std::to_string(conf_id) + " Initiated by: " + initiator);
    }

    void addParticipant(uint32_t conf_id, const std::string& user) {
        participants_[conf_id].push_back(user);
        
        Logger::pcscf(Logger::Level::BEGINNER, "Story: " + user + " is now in the 3-way room.");
        
        Logger::pcscf(Logger::Level::INTERVIEW_C, 
            "Design Choice: Using std::map for O(log n) lookups. In production, "
            "we use 'std::vector::reserve()' to avoid memory fragmentation.");

        Logger::pcscf(Logger::Level::INTERVIEW_T, 
            "3GPP Logic: The AS acts as a B2BUA (Back-to-Back User Agent) to anchor "
            "the media for all three participants.");

        Logger::pcscf(Logger::Level::ENGINEER, 
            "Memory Note: Every push_back into std::vector might trigger a reallocation. "
            "In high-scale telecom, we use 'reserve()' to avoid memory fragmentation.");

        Logger::pcscf(Logger::Level::ENGINEER, 
            "In a real high-scale system, we would use a lock-free hash map.");
            
        // Technical IE simulation for logs
        Logger::pcscf(Logger::Level::ENGINEER, "SIP REFER processed. Target: " + user);
    }

    void listParticipants(uint32_t conf_id) {
        std::string list = "Conf " + std::to_string(conf_id) + " Members: ";
        for (auto& p : participants_[conf_id]) list += p + " ";
        Logger::sys(list);
    }

private:
    // Map of Conference ID to List of Phone Numbers/SIP URIs
    std::map<uint32_t, std::vector<std::string>> participants_;
};