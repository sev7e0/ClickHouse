#pragma once

#include <Core/BackgroundSchedulePool.h>
#include <Core/NamesAndTypes.h>
#include <DataStreams/IBlockOutputStream.h>
#include <Storages/IStorage.h>
#include <Storages/Kafka/ReadBufferFromKafkaConsumer.h>
#include <Storages/Kafka/WriteBufferToKafkaProducer.h>

#include <Poco/Semaphore.h>
#include <ext/shared_ptr_helper.h>

#include <mutex>

namespace DB
{

/** Implements a Kafka queue table engine that can be used as a persistent queue / buffer,
  * or as a basic building block for creating pipelines with a continuous insertion / ETL.
  */
class StorageKafka : public ext::shared_ptr_helper<StorageKafka>, public IStorage
{
public:
    std::string getName() const override { return "Kafka"; }
    std::string getTableName() const override { return table_name; }
    std::string getDatabaseName() const override { return database_name; }

    void startup() override;
    void shutdown() override;

    BlockInputStreams read(
        const Names & column_names,
        const SelectQueryInfo & query_info,
        const Context & context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        unsigned num_streams) override;

    BlockOutputStreamPtr write(
        const ASTPtr & query,
        const Context & context
    ) override;

    void rename(const String & /* new_path_to_db */, const String & new_database_name, const String & new_table_name) override;

    void updateDependencies() override;

    void pushReadBuffer(ConsumerBufferPtr buf);
    ConsumerBufferPtr popReadBuffer();
    ConsumerBufferPtr popReadBuffer(std::chrono::milliseconds timeout);

    ProducerBufferPtr createWriteBuffer();

    const auto & getTopics() const { return topics; }
    const auto & getFormatName() const { return format_name; }
    const auto & getSchemaName() const { return schema_name; }
    const auto & skipBroken() const { return skip_broken; }

protected:
    StorageKafka(
        const std::string & table_name_,
        const std::string & database_name_,
        Context & context_,
        const ColumnsDescription & columns_,
        const String & brokers_, const String & group_, const Names & topics_,
        const String & format_name_, char row_delimiter_, const String & schema_name_,
        size_t num_consumers_, UInt64 max_block_size_, size_t skip_broken,
        bool intermediate_commit_);

private:
    // Configuration and state
    String table_name;
    String database_name;
    Context global_context;
    Names topics;
    const String brokers;
    const String group;
    const String format_name;
    char row_delimiter; /// optional row delimiter for generating char delimited stream in order to make various input stream parsers happy.
    const String schema_name;
    size_t num_consumers; /// total number of consumers
    UInt64 max_block_size; /// maximum block size for insertion into this table

    /// Can differ from num_consumers in case of exception in startup() (or if startup() hasn't been called).
    /// In this case we still need to be able to shutdown() properly.
    size_t num_created_consumers = 0; /// number of actually created consumers.

    Poco::Logger * log;

    // Consumer list
    Poco::Semaphore semaphore;
    std::mutex mutex;
    std::vector<ConsumerBufferPtr> buffers; /// available buffers for Kafka consumers

    size_t skip_broken;

    bool intermediate_commit;

    // Stream thread
    BackgroundSchedulePool::TaskHolder task;
    std::atomic<bool> stream_cancelled{false};

    ConsumerBufferPtr createReadBuffer();

    // Update Kafka configuration with values from CH user configuration.
    void updateConfiguration(cppkafka::Configuration & conf);

    void threadFunc();
    bool streamToViews();
    bool checkDependencies(const String & database_name, const String & table_name);
};

}
