#include "connection.h"
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QThread>

#include "command.h"
#include "scancommand.h"
#include "transporters/defaulttransporter.h"
#include "utils/compat.h"
#include "utils/sync.h"

#ifdef SSH_SUPPORT
#include "sshtransporter.h"
#endif

inline void initResources() { Q_INIT_RESOURCE(lua); }

const QString END_OF_COLLECTION = "end_of_collection";

RedisClient::Connection::Connection(const ConnectionConfig &c, bool autoConnect)
    : m_config(c),
      m_dbNumber(0),
      m_currentMode(Mode::Normal),
      m_autoConnect(autoConnect),
      m_stoppingTransporter(false) {
  initResources();
}

RedisClient::Connection::~Connection() {
  if (isConnected()) disconnect();
}

bool RedisClient::Connection::connect(bool wait) {
  if (isConnected()) return true;

  if (m_config.isValid() == false) throw Exception("Invalid config detected");

  if (m_transporter.isNull()) createTransporter();

  // Create & run transporter
  m_transporterThread = QSharedPointer<QThread>(new QThread);
  m_transporterThread->setObjectName("qredisclient::transporter_thread");
  m_transporter->moveToThread(m_transporterThread.data());

  QObject::connect(m_transporterThread.data(), &QThread::started,
                   m_transporter.data(), &AbstractTransporter::init);
  QObject::connect(m_transporterThread.data(), &QThread::finished,
                   m_transporter.data(),
                   &AbstractTransporter::disconnectFromHost);
  QObject::connect(this, &Connection::shutdownStart, m_transporter.data(),
                   &AbstractTransporter::disconnectFromHost);
  QObject::connect(m_transporter.data(), &AbstractTransporter::connected, this,
                   &Connection::auth);
  QObject::connect(m_transporter.data(), &AbstractTransporter::errorOccurred,
                   this, [this](const QString &err) {
                     disconnect();
                     emit error(QString("Disconnect on error: %1").arg(err));
                   });
  QObject::connect(this, &Connection::authError, this,
                   [this](const QString &) { disconnect(); });

  if (wait) {
    SignalWaiter waiter(m_config.connectionTimeout());
    waiter.addAbortSignal(this, &Connection::shutdownStart);
    waiter.addAbortSignal(m_transporter.data(),
                          &AbstractTransporter::errorOccurred);
    waiter.addAbortSignal(this, &Connection::authError);
    waiter.addSuccessSignal(this, &Connection::authOk);
    m_transporterThread->start();
    return waiter.wait();
  } else {
    m_transporterThread->start();
    return true;
  }
}

bool RedisClient::Connection::isConnected() {
  return m_stoppingTransporter == false && isTransporterRunning();
}

void RedisClient::Connection::disconnect() {
  emit shutdownStart();
  if (isTransporterRunning()) {
    m_stoppingTransporter = true;
    m_transporterThread->quit();
    m_transporterThread->wait();
    m_transporter.clear();
    m_transporterThread.clear();
    m_stoppingTransporter = false;
  }
  m_dbNumber = 0;
}

QFuture<RedisClient::Response> RedisClient::Connection::command(
    const RedisClient::Command &cmd) {
  try {
    return this->runCommand(cmd);
  } catch (RedisClient::Connection::Exception &e) {
    throw Exception("Cannot execute command." + QString(e.what()));
  }
}

QFuture<RedisClient::Response> RedisClient::Connection::command(
    QList<QByteArray> rawCmd, int db) {
  Command cmd(rawCmd, db);

  try {
    return this->runCommand(cmd);
  } catch (RedisClient::Connection::Exception &e) {
    throw Exception("Cannot execute command." + QString(e.what()));
  }
}

QFuture<RedisClient::Response> RedisClient::Connection::command(
    QList<QByteArray> rawCmd, QObject *owner,
    RedisClient::Command::Callback callback, int db) {
  Command cmd(rawCmd, owner, callback, db);

  try {
    return this->runCommand(cmd);
  } catch (RedisClient::Connection::Exception &e) {
    throw Exception("Cannot execute command." + QString(e.what()));
  }
}

RedisClient::Response RedisClient::Connection::commandSync(
    QList<QByteArray> rawCmd, int db) {
  Command cmd(rawCmd, db);
  return commandSync(cmd);
}

RedisClient::Response RedisClient::Connection::commandSync(
    const Command &command) {
  auto future = runCommand(command);

  if (future.isCanceled()) return RedisClient::Response();

  return future.result();
}

QFuture<RedisClient::Response> RedisClient::Connection::runCommand(
    const Command &cmd) {
  if (!cmd.isValid()) throw Exception("Command is not valid");

  if (!isConnected()) {
    if (m_autoConnect) {
      auto d = QSharedPointer<AsyncFuture::Deferred<RedisClient::Response>>(
          new AsyncFuture::Deferred<RedisClient::Response>());

      callAfterConnect([this, cmd, d](const QString &err) {
        if (err.isEmpty()) {
          d->complete(runCommand(cmd));
        } else {
          d->cancel();
        }
      });

      connect(false);

      return d->future();
    } else {
      throw Exception("Try run command in not connected state");
    }
  }

  if (cmd.getOwner() && cmd.getOwner() != this)
    QObject::connect(cmd.getOwner(), SIGNAL(destroyed(QObject *)),
                     m_transporter.data(), SLOT(cancelCommands(QObject *)),
                     static_cast<Qt::ConnectionType>(Qt::QueuedConnection |
                                                     Qt::UniqueConnection));

  auto deferred = cmd.getDeferred();

  emit addCommandToWorker(cmd);

  return deferred.future();
}

bool RedisClient::Connection::waitForIdle(uint timeout) {
  SignalWaiter waiter(timeout);
  waiter.addSuccessSignal(m_transporter.data(),
                          &AbstractTransporter::queueIsEmpty);
  return waiter.wait();
}

QSharedPointer<RedisClient::Connection> RedisClient::Connection::clone() const {
  return QSharedPointer<RedisClient::Connection>(
      new RedisClient::Connection(getConfig()));
}

void RedisClient::Connection::retrieveCollection(
    const ScanCommand &cmd, Connection::CollectionCallback callback) {
  if (!cmd.isValidScanCommand()) throw Exception("Invalid command");

  processScanCommand(cmd, callback);
}

void RedisClient::Connection::retrieveCollectionIncrementally(
    const ScanCommand &cmd,
    RedisClient::Connection::IncrementalCollectionCallback callback) {
  if (!cmd.isValidScanCommand()) throw Exception("Invalid command");

  processScanCommand(
      cmd,
      [callback](QVariant c, QString err) {
        if (err == END_OF_COLLECTION) {
          callback(c, QString(), true);
        } else if (!err.isEmpty()) {
          callback(c, err, true);
        } else {
          callback(c, QString(), false);
        }
      },
      QSharedPointer<QVariantList>(), true);
}

RedisClient::ConnectionConfig RedisClient::Connection::getConfig() const {
  return m_config;
}

void RedisClient::Connection::setConnectionConfig(
    const RedisClient::ConnectionConfig &c) {
  m_config = c;
}

RedisClient::Connection::Mode RedisClient::Connection::mode() const {
  return m_currentMode;
}

int RedisClient::Connection::dbIndex() const { return m_dbNumber; }

double RedisClient::Connection::getServerVersion() {
  return m_serverInfo.version;
}

RedisClient::DatabaseList RedisClient::Connection::getKeyspaceInfo() {
  return m_serverInfo.databases;
}

void RedisClient::Connection::refreshServerInfo() {
  Response infoResult = internalCommandSync({"INFO", "ALL"});
  m_serverInfo = ServerInfo::fromString(infoResult.value().toString());
}

void RedisClient::Connection::getClusterKeys(RawKeysListCallback callback,
                                             const QString &pattern) {
  if (mode() != Mode::Cluster) {
    throw Exception("Connection is not in cluster mode");
  }

  QSharedPointer<RawKeysList> result(new RawKeysList());
  m_notVisitedMasterNodes =
      QSharedPointer<HostList>(new HostList(getMasterNodes()));

  auto onConnect = [this, callback, pattern, result](const QString &err) {
    if (!err.isEmpty()) {
      return callback(*result,
                      QObject::tr("Cannot connect to cluster node %1:%2")
                          .arg(m_config.host())
                          .arg(m_config.port()));
    }

    getDatabaseKeys(m_collectClusterNodeKeys, pattern);
  };

  m_collectClusterNodeKeys = [this, result, callback, onConnect](
                                 const RawKeysList &res, const QString &err) {
    if (!err.isEmpty()) {
      return callback(RawKeysList(), err);
    }

    result->append(res);

    if (!hasNotVisitedClusterNodes()) return callback(*result, QString());

    clusterConnectToNextMasterNode(onConnect);
  };

  clusterConnectToNextMasterNode(onConnect);
}

void RedisClient::Connection::flushDbKeys(
    int dbIndex, std::function<void(const QString &)> callback) {
  if (mode() == Mode::Cluster) {
    m_notVisitedMasterNodes =
        QSharedPointer<HostList>(new HostList(getMasterNodes()));

    auto onConnect = [this, callback](const QString &err) {
      if (!err.isEmpty()) {
        return callback(QObject::tr("Cannot connect to cluster node %1:%2")
                            .arg(m_config.host())
                            .arg(m_config.port()));
      }

      command({"FLUSHDB"}, this, m_cmdCallback);
    };

    m_cmdCallback = [this, callback, dbIndex, onConnect](
                        const RedisClient::Response &, const QString &error) {
      if (!error.isEmpty()) {
        callback(QString(QObject::tr("Cannot flush db (%1): %2"))
                     .arg(dbIndex)
                     .arg(error));
        return;
      }

      if (!hasNotVisitedClusterNodes()) return callback(QString());

      clusterConnectToNextMasterNode(onConnect);
    };

    clusterConnectToNextMasterNode(onConnect);
  } else {
    command(
        {"FLUSHDB"}, this,
        [dbIndex, callback](const RedisClient::Response &,
                            const QString &error) {
          if (!error.isEmpty()) {
            callback(QString(QObject::tr("Cannot flush db (%1): %2"))
                         .arg(dbIndex)
                         .arg(error));
          } else {
            callback(QString());
          }
        },
        dbIndex);
  }
}

void RedisClient::Connection::getDatabaseKeys(RawKeysListCallback callback,
                                              const QString &pattern,
                                              int dbIndex, long scanLimit) {
  QList<QByteArray> rawCmd{"scan",  "0",
                           "MATCH", pattern.toUtf8(),
                           "COUNT", QString::number(scanLimit).toLatin1()};
  ScanCommand keyCmd(rawCmd, dbIndex);

  retrieveCollection(keyCmd, [callback](QVariant r, QString err) {
    if (!err.isEmpty())
      return callback(RawKeysList(), QString("Cannot load keys: %1").arg(err));

    auto keysList = convertQVariantList(r.toList());

    return callback(keysList, QString());
  });
}

void RedisClient::Connection::getNamespaceItems(
    RedisClient::Connection::NamespaceItemsCallback callback,
    const QString &nsSeparator, const QString &filter, int dbIndex) {
  QFile script("://scan.lua");
  if (!script.open(QIODevice::ReadOnly)) {
    qWarning() << "Cannot open LUA resource";
    return;
  }

  QByteArray LUA_SCRIPT = script.readAll();

  QList<QByteArray> rawCmd{"eval", LUA_SCRIPT, "0", nsSeparator.toUtf8(),
                           filter.toUtf8()};

  Command evalCmd(rawCmd, dbIndex);

  evalCmd.setCallBack(this, [callback](RedisClient::Response r, QString error) {
    if (!error.isEmpty()) {
      return callback(NamespaceItems(), error);
    }

    QList<QVariant> result = r.value().toList();

    if (result.size() != 2) {
      return callback(NamespaceItems(), "Invalid response from LUA script");
    }

    QJsonDocument rootNamespacesJson =
        QJsonDocument::fromJson(result[0].toByteArray());
    QJsonDocument rootKeysJson =
        QJsonDocument::fromJson(result[1].toByteArray());

    if (rootNamespacesJson.isEmpty() || rootKeysJson.isEmpty() ||
        !(rootNamespacesJson.isObject() && rootKeysJson.isObject())) {
      return callback(NamespaceItems(), "Invalid response from LUA script");
    }

    QVariantMap rootNamespaces = rootNamespacesJson.toVariant().toMap();
    QList<QString> rootKeys = rootKeysJson.toVariant().toMap().keys();

    QVariantMap::const_iterator i = rootNamespaces.constBegin();
    RootNamespaces rootNs;
    rootNs.reserve(rootNamespaces.size());

    while (i != rootNamespaces.constEnd()) {
      rootNs.append(QPair<QByteArray, ulong>(i.key().toUtf8(),
                                             (ulong)i.value().toDouble()));
      ++i;
    }

    RootKeys keys;
    keys.reserve(rootKeys.size());

    foreach (QString key, rootKeys) { keys.append(key.toUtf8()); }

    callback(NamespaceItems(rootNs, keys), QString());
  });

  runCommand(evalCmd);
}

void RedisClient::Connection::createTransporter() {
  // todo : implement unix socket transporter
  if (m_config.useSshTunnel()) {
#ifdef SSH_SUPPORT
    m_transporter =
        QSharedPointer<AbstractTransporter>(new SshTransporter(this));
#else
    throw SSHSupportException("QRedisClient compiled without ssh support.");
#endif
  } else {
    m_transporter =
        QSharedPointer<AbstractTransporter>(new DefaultTransporter(this));
  }
}

bool RedisClient::Connection::isTransporterRunning() {
  return m_transporter && m_transporterThread &&
         m_transporterThread->isRunning();
}

RedisClient::Response RedisClient::Connection::internalCommandSync(
    QList<QByteArray> rawCmd) {
  Command cmd(rawCmd);
  cmd.markAsHiPriorityCommand();
  return commandSync(cmd);
}

void RedisClient::Connection::processScanCommand(
    const ScanCommand &cmd, CollectionCallback callback,
    QSharedPointer<QVariantList> result, bool incrementalProcessing) {
  if (result.isNull())
    result = QSharedPointer<QVariantList>(new QVariantList());

  auto cmdWithCallback = cmd;

  cmdWithCallback.setCallBack(
      this, [this, cmd, result, callback, incrementalProcessing](
                RedisClient::Response r, QString error) {
        if (r.isErrorMessage()) {
          /*
           * aliyun cloud provides iscan command for scanning clusters
           */
          if (cmd.getPartAsString(0).toLower() == "scan" &&
              r.isDisabledCommandErrorMessage()) {
            auto rawCmd = cmd.getSplitedRepresentattion();
            rawCmd.replace(0, "iscan");
            auto iscanCmd = ScanCommand(rawCmd);
            return processScanCommand(iscanCmd, callback, result,
                                      incrementalProcessing);
          }

          callback(r.value(), r.value().toString());
          return;
        }

        if (!error.isEmpty()) {
          callback(QVariant(), error);
          return;
        }

        if (incrementalProcessing) result->clear();

        if (!r.isValidScanResponse()) {
          if (result->isEmpty())
            callback(QVariant(),
                     incrementalProcessing ? END_OF_COLLECTION : QString());
          else
            callback(QVariant(*result), QString());

          return;
        }

        result->append(r.getCollection());

        if (r.getCursor() <= 0) {
          callback(QVariant(*result),
                   incrementalProcessing ? END_OF_COLLECTION : QString());
          return;
        }

        auto newCmd = cmd;
        newCmd.setCursor(r.getCursor());

        processScanCommand(newCmd, callback, result);
      });

  runCommand(cmdWithCallback);
}

void RedisClient::Connection::changeCurrentDbNumber(int db) {
  if (m_dbNumberMutex.tryLock(5000)) {
    m_dbNumber = db;
    m_dbNumberMutex.unlock();
  } else {
    qWarning() << "Cannot lock db number mutex!";
  }
}

void RedisClient::Connection::clusterConnectToNextMasterNode(
    std::function<void(const QString &err)> callback) {
  if (!hasNotVisitedClusterNodes()) {
    return;
  }

  Host h = m_notVisitedMasterNodes->first();
  m_notVisitedMasterNodes->removeFirst();

  callAfterConnect(callback);

  if (m_config.overrideClusterHost()) {
    reconnectTo(h.first, h.second);
  } else {
    reconnectTo(m_config.host(), h.second);
  }
}

bool RedisClient::Connection::hasNotVisitedClusterNodes() const {
  return m_notVisitedMasterNodes && m_notVisitedMasterNodes->size() > 0;
}

void RedisClient::Connection::callAfterConnect(
    std::function<void(const QString &err)> callback) {
  auto context = new QObject();

  QObject::connect(this, &Connection::authOk, context, [callback, context]() {
    callback(QString());
    context->deleteLater();
  });
  QObject::connect(this, &Connection::error, context,
                   [callback, context](const QString &err) {
                     callback(err);
                     context->deleteLater();
                   });
}

RedisClient::Connection::HostList RedisClient::Connection::getMasterNodes() {
  HostList result;

  if (mode() != Mode::Cluster) {
    return result;
  }

  Response r;

  try {
    r = internalCommandSync({"CLUSTER", "SLOTS"});
  } catch (const Exception &e) {
    emit error(QString("Cannot retrive nodes list").arg(e.what()));
    return result;
  }

  QVariantList slotsList = r.value().toList();

  foreach (QVariant clusterSlot, slotsList) {
    QVariantList details = clusterSlot.toList();

    if (details.size() < 3) continue;

    QVariantList masterDetails = details[2].toList();

    result.append({masterDetails[0].toString(), masterDetails[1].toInt()});
  }

  return result;
}

QFuture<bool> RedisClient::Connection::isCommandSupported(
    QList<QByteArray> rawCmd) {
  auto d = QSharedPointer<AsyncFuture::Deferred<bool>>(
      new AsyncFuture::Deferred<bool>());

  cmd(
      rawCmd, this, -1,
      [d](RedisClient::Response r) {
        d->complete(!r.isDisabledCommandErrorMessage());
      },
      [d](const QString &err) {
        d->complete(!err.contains("unknown command"));
      });

  return d->future();
}

void RedisClient::Connection::auth() {
  emit log("AUTH");

  try {
    if (m_config.useAuth()) {
      internalCommandSync({"AUTH", m_config.auth().toUtf8()});
    }

    Response testResult = internalCommandSync({"PING"});

    if (testResult.value().toByteArray() != QByteArray("PONG")) {
      emit authError("Redis server requires password or password is not valid");
      emit error("AUTH ERROR");
      return;
    }

    refreshServerInfo();

    // TODO(u_glide): add option to disable automatic mode switching
    if (m_serverInfo.clusterMode) {
      m_currentMode = Mode::Cluster;
      emit log("Cluster detected");
    } else if (m_serverInfo.sentinelMode) {
      m_currentMode = Mode::Sentinel;
      emit log("Sentinel detected. Requesting master node...");

      Response mastersResult = internalCommandSync({"SENTINEL", "masters"});

      if (!mastersResult.isArray()) {
        emit error(QString(
            "Connection error: cannot retrive master node from sentinel"));
        return;
      }

      QVariantList result = mastersResult.value().toList();

      if (result.size() == 0) {
        emit error(QString("Connection error: invalid response from sentinel"));
        return;
      }

      QStringList masterInfo = result.at(0).toStringList();

      if (masterInfo.size() < 6) {
        emit error(QString("Connection error: invalid response from sentinel"));
        return;
      }

      QString host = masterInfo[3];

      if (!m_config.useSshTunnel() &&
          (host == "127.0.0.1" || host == "localhost"))
        host = m_config.host();

      emit reconnectTo(host, masterInfo[5].toInt());
      return;
    }

    emit log("Connected");
    emit authOk();
    emit connected();
  } catch (const Exception &e) {
    emit error(QString("Connection error on AUTH: %1").arg(e.what()));
    emit authError("Connection error on AUTH");
  }
}

void RedisClient::Connection::setTransporter(
    QSharedPointer<RedisClient::AbstractTransporter> transporter) {
  if (transporter.isNull()) return;

  m_transporter = transporter;
}

QSharedPointer<RedisClient::AbstractTransporter>
RedisClient::Connection::getTransporter() const {
  return m_transporter;
}

RedisClient::ServerInfo::ServerInfo()
    : version(0.0), clusterMode(false), sentinelMode(false) {}

RedisClient::ServerInfo RedisClient::ServerInfo::fromString(
    const QString &info) {
  QStringList lines = info.split("\r\n");

  ParsedServerInfo parsed;
  QString currentSection{"unknown"};
  int posOfSeparator = -1;

  foreach (QString line, lines) {
    if (line.startsWith("#")) {
      currentSection = line.mid(2).toLower();
      continue;
    }

    posOfSeparator = line.indexOf(':');

    if (posOfSeparator == -1) continue;

    parsed[currentSection][line.mid(0, posOfSeparator)] =
        line.mid(posOfSeparator + 1);
  }

  QRegExp versionRegex("redis_version:([0-9]+\\.[0-9]+)", Qt::CaseInsensitive,
                       QRegExp::RegExp2);
  QRegExp modeRegex("redis_mode:([a-z]+)", Qt::CaseInsensitive,
                    QRegExp::RegExp2);

  RedisClient::ServerInfo result;
  result.parsed = parsed;
  result.version =
      (versionRegex.indexIn(info) == -1) ? 0.0 : versionRegex.cap(1).toDouble();

  if (modeRegex.indexIn(info) != -1) {
    if (modeRegex.cap(1) == "cluster") result.clusterMode = true;
    if (modeRegex.cap(1) == "sentinel") result.sentinelMode = true;
  }

  if (result.clusterMode) {
    result.databases.insert(0, 0);
    return result;
  } else if (result.sentinelMode) {
    return result;
  }

  // Parse keyspace info
  QRegularExpression getDbAndKeysCount("^db(\\d+):keys=(\\d+).*");
  getDbAndKeysCount.setPatternOptions(QRegularExpression::MultilineOption);
  QRegularExpressionMatchIterator iter = getDbAndKeysCount.globalMatch(info);
  while (iter.hasNext()) {
    QRegularExpressionMatch match = iter.next();
    int dbIndex = match.captured(1).toInt();
    result.databases.insert(dbIndex, match.captured(2).toInt());
  }

  if (result.databases.size() == 0) return result;

  int lastKnownDbIndex = result.databases.lastKey();
  for (int dbIndex = 0; dbIndex < lastKnownDbIndex; ++dbIndex) {
    if (!result.databases.contains(dbIndex)) {
      result.databases.insert(dbIndex, 0);
    }
  }

  return result;
}

QVariantMap RedisClient::ServerInfo::ParsedServerInfo::toVariantMap() {
  QVariantMap categories;
  QHashIterator<QString, QHash<QString, QString>> catIterator(*this);

  while (catIterator.hasNext()) {
    catIterator.next();
    QHashIterator<QString, QString> propIterator(catIterator.value());
    QVariantMap properties;

    while (propIterator.hasNext()) {
      propIterator.next();
      properties.insert(propIterator.key(), propIterator.value());
    }

    categories.insert(catIterator.key(), properties);
  }

  return categories;
}

RedisClient::Connection::SSHSupportException::SSHSupportException(
    const QString &e)
    : Connection::Exception(e) {}
