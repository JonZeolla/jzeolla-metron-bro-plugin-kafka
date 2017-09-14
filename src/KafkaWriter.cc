/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KafkaWriter.h"

using namespace logging;
using namespace writer;

KafkaWriter::KafkaWriter(WriterFrontend* frontend): WriterBackend(frontend), formatter(NULL), producer(NULL), topic(NULL)
{
  // need thread-local copies of all user-defined settings coming from
  // bro scripting land.  accessing these is not thread-safe and 'DoInit'
  // is potentially accessed from multiple threads.

  // tag_json - thread local copy
  tag_json = BifConst::Kafka::tag_json;

  // topic name - thread local copy
  topic_name.assign(
    (const char*)BifConst::Kafka::topic_name->Bytes(),
    BifConst::Kafka::topic_name->Len());

  // kafka_conf - thread local copy
  Val* val = BifConst::Kafka::kafka_conf->AsTableVal();
  IterCookie* c = val->AsTable()->InitForIteration();
  HashKey* k;
  TableEntryVal* v;
  while ((v = val->AsTable()->NextEntry(k, c))) {

      // fetch the key and value
      ListVal* index = val->AsTableVal()->RecoverIndex(k);
      string key = index->Index(0)->AsString()->CheckString();
      string val = v->Value()->AsString()->CheckString();
      kafka_conf.insert (kafka_conf.begin(), pair<string, string> (key, val));

      // cleanup
      Unref(index);
      delete k;
  }
}

KafkaWriter::~KafkaWriter()
{}

bool KafkaWriter::DoInit(const WriterInfo& info, int num_fields, const threading::Field* const* fields)
{
    // if no global 'topic_name' is defined, use the log stream's 'path'
    if(topic_name.empty()) {
        topic_name = info.path;
    }

    // initialize the formatter
    if(BifConst::Kafka::tag_json) {
      formatter = new threading::formatter::TaggedJSON(info.path, this, threading::formatter::JSON::TS_EPOCH);
    } else {
      formatter = new threading::formatter::JSON(this, threading::formatter::JSON::TS_EPOCH);
    }

    // is debug enabled
    string debug;
    debug.assign((const char*)BifConst::Kafka::debug->Bytes(), BifConst::Kafka::debug->Len());
    bool is_debug(!debug.empty());
    if(is_debug) {
      MsgThread::Info(Fmt("Debug is turned on and set to: %s.  Available debug context: %s.", debug.c_str(), RdKafka::get_debug_contexts().c_str()));
    }
    else {
      MsgThread::Info(Fmt("Debug is turned off."));
    }

    // kafka global configuration
    string err;
    conf = RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL);

    // apply the user-defined settings to kafka
    map<string,string>::iterator i;
    for (i = kafka_conf.begin(); i != kafka_conf.end(); ++i) {
      string key = i->first;
      string val = i->second;

      // apply setting to kafka
      if (RdKafka::Conf::CONF_OK != conf->set(key, val, err)) {
          Error(Fmt("Failed to set '%s'='%s': %s", key.c_str(), val.c_str(), err.c_str()));
          return false;
      }
    }

    if(is_debug) {
        string key("debug");
        string val(debug);
        if (RdKafka::Conf::CONF_OK != conf->set(key, val, err)) {
            Error(Fmt("Failed to set '%s'='%s': %s", key.c_str(), val.c_str(), err.c_str()));
            return false;
        }
    }

    // create kafka producer
    producer = RdKafka::Producer::create(conf, err);
    if (!producer) {
        Error(Fmt("Failed to create producer: %s", err.c_str()));
        return false;
    }

    // create handle to topic
    topic_conf = RdKafka::Conf::create(RdKafka::Conf::CONF_TOPIC);
    topic = RdKafka::Topic::create(producer, topic_name, topic_conf, err);
    if (!topic) {
        Error(Fmt("Failed to create topic handle: %s", err.c_str()));
        return false;
    }

    if(is_debug) {
        MsgThread::Info(Fmt("Successfully created producer."));
    }

    return true;
}

/**
 * Writer-specific method called just before the threading system is
 * going to shutdown. It is assumed that once this messages returns,
 * the thread can be safely terminated.
 */
bool KafkaWriter::DoFinish(double network_time)
{
    bool success = false;
    int poll_interval = 1000;
    int waited = 0;
    int max_wait = BifConst::Kafka::max_wait_on_shutdown;

    // wait a bit for queued messages to be delivered
    while (producer->outq_len() > 0 && waited <= max_wait) {
        producer->poll(poll_interval);
        waited += poll_interval;
    }

    // successful only if all messages delivered
    if (producer->outq_len() == 0) {
        success = true;
    } else {
        Error(Fmt("Unable to deliver %0d message(s)", producer->outq_len()));
    }

    delete topic;
    delete producer;
    delete formatter;

    return success;
}

/**
 * Writer-specific output method implementing recording of one log
 * entry.
 */
bool KafkaWriter::DoWrite(int num_fields, const threading::Field* const* fields, threading::Value** vals)
{
    ODesc buff;
    buff.Clear();

    // format the log entry
    formatter->Describe(&buff, num_fields, fields, vals);

    // send the formatted log entry to kafka
    const char* raw = (const char*)buff.Bytes();
    RdKafka::ErrorCode resp = producer->produce(
        topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
        const_cast<char*>(raw), strlen(raw), NULL, NULL);

    if (RdKafka::ERR_NO_ERROR == resp) {
        producer->poll(0);
    }
    else {
        string err = RdKafka::err2str(resp);
        Error(Fmt("Kafka send failed: %s", err.c_str()));
    }

    return true;
}

/**
 * Writer-specific method implementing a change of fthe buffering
 * state.	If buffering is disabled, the writer should attempt to
 * write out information as quickly as possible even if doing so may
 * have a performance impact. If enabled (which is the default), it
 * may buffer data as helpful and write it out later in a way
 * optimized for performance. The current buffering state can be
 * queried via IsBuf().
 */
bool KafkaWriter::DoSetBuf(bool enabled)
{
    // no change in behavior
    return true;
}

/**
 * Writer-specific method implementing flushing of its output.	A writer
 * implementation must override this method but it can just
 * ignore calls if flushing doesn't align with its semantics.
 */
bool KafkaWriter::DoFlush(double network_time)
{
    producer->poll(0);
    return true;
}

/**
 * Writer-specific method implementing log rotation.	Most directly
 * this only applies to writers writing into files, which should then
 * close the current file and open a new one.	However, a writer may
 * also trigger other apppropiate actions if semantics are similar.
 * Once rotation has finished, the implementation *must* call
 * FinishedRotation() to signal the log manager that potential
 * postprocessors can now run.
 */
bool KafkaWriter::DoRotate(const char* rotated_path, double open, double close, bool terminating)
{
    // no need to perform log rotation
    return FinishedRotation();
}

/**
 * Triggered by regular heartbeat messages from the main thread.
 */
bool KafkaWriter::DoHeartbeat(double network_time, double current_time)
{
    producer->poll(0);
    return true;
}
