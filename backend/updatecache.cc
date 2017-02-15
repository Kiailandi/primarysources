// Copyright 2015 Google Inc. All Rights Reserved.
// Author: Sebastian Schaffert <schaffert@google.com>

#include <ctime>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <cppcms/json.h>
#include <persistence/Persistence.h>
#include <service/RedisCacheService.h>
#include <util/ProgressBar.h>
#include <util/TimeLogger.h>

DEFINE_string(c, "", "backend configuration file to read database and Redis configuration");
DEFINE_string(mode, "update", "cache update mode (update or clear)");

using wikidata::primarysources::TimeLogger;
using wikidata::primarysources::ProgressBar;
using wikidata::primarysources::Persistence;
using wikidata::primarysources::RedisCacheService;

using wikidata::primarysources::model::ApprovalState ;
using wikidata::primarysources::model::Statement;
using wikidata::primarysources::model::Statements;

namespace {
// Create cache key for an entity and dataset; the cache key is used to cache
// all statements of the given dataset having the entity as subject. If dataset
// is "", the cache key refers to all statements and the dataset name "all" will
// be used.
std::string createCacheKey(const std::string &qid, ApprovalState state, const std::string &dataset) {
    std::string result;
    if (dataset == "") {
        result.reserve(qid.size() + 2);
        result += qid;
        result += "-";
        result += std::to_string(state);
    } else {
        result.reserve(qid.size() + dataset.size() + 3);
        result += qid;
        result += "-";
        result += dataset;
        result += "-";
        result += std::to_string(state);
    }
    return result;
}
}  // namespace

int main(int argc, char **argv) {
    google::InitGoogleLogging(argv[0]);
    google::gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::gflags::SetUsageMessage(
            std::string("Bulk-load or clear Redis cache. Usage: \n") +
            argv[0] + " -c configfile -mode [update|clear]");

    if (FLAGS_c == "") {
        google::gflags::ShowUsageWithFlags(argv[0]);
        std::cerr << "Option -c is required." << std::endl << std::endl;
        return 1;
    }

    // read configuration
    cppcms::json::value config;
    std::ifstream cfgfile(FLAGS_c);
    cfgfile >> config;

    RedisCacheService redis(config["redis"]["host"].str(),
                            config["redis"]["port"].number());

    if (FLAGS_mode == "clear") {
        TimeLogger timer("Clearing cached Redis entries");
        redis.Clear();
        return 0;
    }

    if (FLAGS_mode != "update") {
        LOG(ERROR) << "unknown mode: " << FLAGS_mode;
        return 1;
    }

    try {
        cppdb::session sql(
                wikidata::primarysources::build_connection(config["database"]));

        sql.begin();
        wikidata::primarysources::Persistence p(sql, true);

        LOG(INFO) << "Start refreshing all cached Redis entries ...";

        auto datasets = p.getDatasets();
        datasets.insert(datasets.begin(), "");

        long count = 0;
        for (const auto& dataset : datasets) {
            TimeLogger timer("Refreshing cached Redis entries for " +
                             (dataset == "" ? "all datasets" : "dataset " + dataset));

            std::cout << "Updating Redis entries for " <<
                    (dataset == "" ? "all datasets" : "dataset " + dataset) << std::endl;

            Statements stmts;
            std::string qid = "";

            int64_t size = p.countStatements(ApprovalState::UNAPPROVED, dataset);
            ProgressBar progress(70, size);
            progress.Update(0);

            p.getAllStatements([&](const Statement& s) {
                if (qid != s.qid() && qid != "") {
                    // store current batch of statements and clear it
                    redis.Add(createCacheKey(qid, ApprovalState::UNAPPROVED, dataset), stmts);
                    stmts.clear_statements();
                    count++;

                    if (count % (size/100) == 0) {
                        progress.Update(count);
                    }
                }
                *stmts.add_statements() = s;
                qid = s.qid();
            }, ApprovalState::UNAPPROVED, dataset);

            if (stmts.statements_size() > 0) {
                redis.Add(createCacheKey(qid, ApprovalState::UNAPPROVED, dataset), stmts);
            }
            progress.Update(size);
        }

        LOG(INFO) << "Finished refreshing all cached Redis entries.";

        sql.commit();

        return 0;
    } catch (std::exception const &e) {
        LOG(ERROR) << e.what();

        return 1;
    }
}
