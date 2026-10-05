/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "smsplugin.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QLoggingCategory>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <KPluginFactory>

#include <core/networkpacket.h>


K_PLUGIN_CLASS_WITH_JSON(SmsPlugin, "sailfishconnect_sms.json")

static Q_LOGGING_CATEGORY(logger, "sailfishconnect.sms")

static const QString PACKET_TYPE_REQUEST_CONVERSATIONS = QStringLiteral("kdeconnect.sms.request_conversations");
static const QString PACKET_TYPE_REQUEST_CONVERSATION = QStringLiteral("kdeconnect.sms.request_conversation");
static const QString PACKET_TYPE_MESSAGES = QStringLiteral("kdeconnect.sms.messages");

// CommHistory::Event::EventType: SMSEvent, MMSEvent, ClassZeroSMSEvent, see: libcommhistory/src/event.h
static const QString TEXT_EVENTS = QStringLiteral("type IN (2, 6, 7) AND isDraft = 0");

static const QString SELECT_MESSAGES = QStringLiteral(
    "SELECT id, groupId, startTime, direction, status, isRead, freeText, remoteUid "
    "FROM Events WHERE ") + TEXT_EVENTS + QLatin1Char(' ');

// The newest message of every thread
static const QString LATEST_PER_THREAD = QStringLiteral(
    "AND id = (SELECT e.id FROM Events e WHERE e.groupId = Events.groupId AND ")
    + TEXT_EVENTS + QStringLiteral(" ORDER BY e.startTime DESC, e.id DESC LIMIT 1)");

// Message types of the KDE Connect protocol, as in Android's Telephony.Sms
enum MessageType { Inbox = 1, Sent = 2, Outbox = 4, Failed = 5 };

// CommHistory::Event::EventDirection and EventStatus
enum { InboundDirection = 1, SendingStatus = 1, FailedStatus = 4 };

// Whoever commits to commhistory.db announces it on the session bus
static const QString COMMHISTORY_PATH = QStringLiteral("/CommHistoryModel");
static const QString COMMHISTORY_INTERFACE = QStringLiteral("com.nokia.commhistory");

static QString databasePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/commhistory/commhistory.db");
}

static QVariantMap toMessage(const QSqlQuery &query)
{
    int type = Sent;
    if (query.value(3).toInt() == InboundDirection) {
        type = Inbox;
    } else if (query.value(4).toInt() >= FailedStatus) {
        type = Failed;
    } else if (query.value(4).toInt() == SendingStatus) {
        type = Outbox;
    }

    const QString body = query.value(6).toString();
    return {
        { QStringLiteral("_id"), query.value(0) },
        { QStringLiteral("thread_id"), query.value(1) },
        // Whole seconds are not unique: the computer stores a thread's messages by date and
        // drops all but one of those sharing it, so the id spreads them over the milliseconds
        { QStringLiteral("date"), query.value(2).toLongLong() * 1000 + query.value(0).toLongLong() % 1000 },
        { QStringLiteral("type"), type },
        { QStringLiteral("read"), query.value(5).toInt() },
        // EventTextMessage: the message has a body
        { QStringLiteral("event"), body.isEmpty() ? 0 : 1 },
        { QStringLiteral("body"), body },
        { QStringLiteral("addresses"), QVariantList {
            QVariantMap { { QStringLiteral("address"), query.value(7).toString() } } } },
    };
}

// Opens commhistory.db read-only for this one query.
// An empty result is either an empty table or a failed read; *ok tells which.
static QVariantList fetchMessages(const QString &condition, const QVariantList &binds, bool *ok = nullptr)
{
    if (ok) {
        *ok = false;
    }
    const QString connection = QStringLiteral("sailfishconnect_sms");
    QVariantList messages;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath());
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));

        if (!db.open()) {
            qCWarning(logger) << "cannot open" << databasePath() << db.lastError().text();
        } else {
            QSqlQuery query(db);
            query.prepare(SELECT_MESSAGES + condition);
            for (const QVariant &bind : binds) {
                query.addBindValue(bind);
            }

            if (!query.exec()) {
                qCWarning(logger) << "query failed:" << query.lastError().text();
            } else {
                if (ok) {
                    *ok = true;
                }
                while (query.next()) {
                    messages.append(toMessage(query));
                }
            }
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return messages;
}

static qint64 idOf(const QVariant &message)
{
    return message.toMap().value(QStringLiteral("_id")).toLongLong();
}

SmsPlugin::SmsPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    seedLastEventId();

    // The signal's payload is not read; it only says that something was committed
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.connect(QString(), COMMHISTORY_PATH, COMMHISTORY_INTERFACE,
                     QStringLiteral("eventsAdded"), this, SLOT(sendNewMessages()))) {
        qCWarning(logger) << "cannot listen for new messages:" << bus.lastError().message();
    }
}

void SmsPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.type() == PACKET_TYPE_REQUEST_CONVERSATIONS) {
        sendMessages(fetchMessages(LATEST_PER_THREAD, {}));
        return;
    }

    if (np.type() == PACKET_TYPE_REQUEST_CONVERSATION) {
        // The computer asks for the messages older than the oldest it holds;
        // a negative start means the newest ones.
        const qint64 rangeStart = np.get<qint64>(QStringLiteral("rangeStartTimestamp"), -1);
        QVariantList binds { np.get<qint64>(QStringLiteral("threadID")) };
        QString condition = QStringLiteral("AND groupId = ? ");
        if (rangeStart >= 0) {
            condition += QStringLiteral("AND startTime <= ? ");
            binds.append(rangeStart / 1000);
        }
        condition += QStringLiteral("ORDER BY startTime DESC, id DESC LIMIT ?");
        binds.append(np.get<qint64>(QStringLiteral("numberToRequest"), 10));

        sendMessages(fetchMessages(condition, binds));
    }
}

bool SmsPlugin::sendMessages(const QVariantList &messages)
{
    NetworkPacket np(PACKET_TYPE_MESSAGES, {
        { QStringLiteral("version"), 2 },
        { QStringLiteral("messages"), messages },
    });
    return sendPacket(np);
}

// Starts from the newest message that exists now, so only later ones are pushed.
// A failed read leaves m_seeded unset, and the next scan seeds instead of pushing.
void SmsPlugin::seedLastEventId()
{
    const QVariantList newest = fetchMessages(QStringLiteral("ORDER BY id DESC LIMIT 1"), {}, &m_seeded);
    if (!newest.isEmpty()) {
        m_lastEventId = idOf(newest.first());
    }
}

void SmsPlugin::sendNewMessages()
{
    if (!m_seeded) {
        seedLastEventId();
        return;
    }

    const QVariantList messages = fetchMessages(
        QStringLiteral("AND id > ? ORDER BY id"), { m_lastEventId });
    if (!messages.isEmpty() && sendMessages(messages)) {
        m_lastEventId = idOf(messages.last());
    }
}

#include "smsplugin.moc"
