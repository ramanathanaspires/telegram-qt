/*
    Copyright (C) 2014 Alexandr Akulich <akulichalexander@gmail.com>

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
    LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
    OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
    WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#include "CTelegramCore.hpp"

#include <QFile>
#include <QBuffer>
#include <QDebug>

#include <QDateTime>

#include <QtEndian>

#include "CTelegramStream.hpp"
#include "CTelegramTransport.hpp"
#include "Utils.hpp"

CTelegramCore::CTelegramCore(QObject *parent) :
    QObject(parent),
    m_appId(0),
    m_appHash(QLatin1String("00000000000000000000000000000000")),
    m_transport(0),
    m_serverPublicFingersprint(0)
{
    Utils::randomBytes(m_clientNonce.data, m_clientNonce.size());

    m_rsaKey = Utils::loadKey();
}

void CTelegramCore::setAppId(quint32 newId)
{
    m_appId = newId;
}

bool CTelegramCore::setAppHash(const QString &newHash)
{
    if (newHash.length() != 16)
        return false;

    m_appHash = newHash;

    return true;
}

void CTelegramCore::setTransport(CTelegramTransport *newTransport)
{
    m_transport = newTransport;

    connect(m_transport, SIGNAL(readyRead()), SLOT(whenReadyRead()));
}

quint64 CTelegramCore::formatTimeStamp(qint64 timeInMs)
{
    static const quint64 maxMsecValue = (1UL << 32) - 1;

    quint64 secs = timeInMs / 1000;
    quint64 msecs = maxMsecValue / 1000 * (timeInMs % 1000);

    return (secs << 32) + msecs;
}

quint64 CTelegramCore::timeStampToMSecsSinceEpoch(quint64 ts)
{
    static const quint64 maxMsecValue = (1UL << 32) - 1;

    quint64 secs = ts >> 32;
    quint64 msecs = ts & maxMsecValue;

    msecs = msecs * 10000 / maxMsecValue;

    if (msecs % 10 >= 5) {
        msecs += 5;
    }
    msecs /= 10;

    return secs * 1000 + msecs;
}

void CTelegramCore::requestPqAuthorization()
{
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    CTelegramStream outputStream(&output);

    outputStream << ReqPQ;
    outputStream << m_clientNonce;

    sendPackage(output.buffer());
}

bool CTelegramCore::answerPqAuthorization(const QByteArray &payload)
{
    // Payload is passed as const, but we open device in read-only mode, so
    // Let's workaround const by construction variable copy with COW-feature.
    QByteArray data = payload;
    QBuffer input;
    input.setBuffer(&data);
    input.open(QIODevice::ReadOnly);
    CTelegramStream inputStream(&input);

    TLValues responsePqValue;
    inputStream >> responsePqValue;

    if (responsePqValue != ResPQ) {
        qDebug() << "Error: Unexpected operation code";
        return false;
    }

    TLNumber128 clientNonce;
    TLNumber128 serverNonce;

    inputStream >> clientNonce;

    if (clientNonce != m_clientNonce) {
        qDebug() << "Error: Client nonce in incoming package is different from our own.";
        return false;
    }

    inputStream >> serverNonce;

    m_serverNonce = serverNonce;

    QByteArray pq;

    inputStream >> pq;

    if (pq.size() != 8) {
        qDebug() << "Error: PQ should be 8 bytes in length";
        return false;
    }

    m_pq = qFromBigEndian<quint64>((uchar *) pq.data());

    quint64 div1 = Utils::findDivider(m_pq);

    if (div1 == 1) {
        qDebug() << "Error: Can not solve PQ.";
        return false;
    }
    quint64 div2 = m_pq / div1;

    if (div1 < div2) {
        m_p = div1;
        m_q = div2;
    } else {
        m_p = div2;
        m_q = div1;
    }

    QVector<quint64> fingersprints;

    inputStream >> fingersprints;

    if (fingersprints.count() != 1) {
        qDebug() << "Error: Unexpected Server RSA Fingersprints vector.";
        return false;
    }

    m_serverPublicFingersprint = fingersprints.at(0);

    emit pqReceived();

    if (m_rsaKey.fingersprint != m_serverPublicFingersprint) {
        qDebug() << "Error: Server RSA Fingersprint does not match to loaded key";
        return false;
    }

    return true;
}

void CTelegramCore::requestDhParameters()
{
    Utils::randomBytes(m_newNonce.data, m_newNonce.size());

    QByteArray bigEndianNumber;
    bigEndianNumber.fill(char(0), 8);

    QByteArray encryptedPackage;
    {
        static const int requestedEncryptedPackageLength = 255;
        QBuffer innerData;
        innerData.open(QIODevice::WriteOnly);
        CTelegramStream encryptedStream(&innerData);

        encryptedStream << PQInnerData;

        qToBigEndian(m_pq, (uchar *) bigEndianNumber.data());
        encryptedStream << bigEndianNumber;

        bigEndianNumber.fill(char(0), 4);
        qToBigEndian(m_p, (uchar *) bigEndianNumber.data());
        encryptedStream << bigEndianNumber;

        qToBigEndian(m_q, (uchar *) bigEndianNumber.data());
        encryptedStream << bigEndianNumber;

        encryptedStream << m_clientNonce;
        encryptedStream << m_serverNonce;
        encryptedStream << m_newNonce;

        QByteArray sha = Utils::sha1(innerData.data());
        QByteArray randomPadding;
        randomPadding.resize(requestedEncryptedPackageLength - (sha.length() + innerData.data().length()));
        Utils::randomBytes(randomPadding.data(), randomPadding.size());

        encryptedPackage = Utils::rsa(sha + innerData.data() + randomPadding, m_rsaKey);
    }

    QBuffer output;
    output.open(QIODevice::WriteOnly);
    CTelegramStream outputStream(&output);

    outputStream << ReqDHParams;
    outputStream << m_clientNonce;
    outputStream << m_serverNonce;

    bigEndianNumber.fill(char(0), 4);
    qToBigEndian(m_p, (uchar *) bigEndianNumber.data());
    outputStream << bigEndianNumber;

    qToBigEndian(m_q, (uchar *) bigEndianNumber.data());
    outputStream << bigEndianNumber;

    outputStream << m_serverPublicFingersprint;

    outputStream << encryptedPackage;

    sendPackage(output.buffer());
}

void CTelegramCore::whenReadyRead()
{
    QByteArray incoming = m_transport->getPackage();
    QBuffer input;
    input.setBuffer(&incoming);
    input.open(QIODevice::ReadOnly);
    CTelegramStream inputStream(&input);

    quint64 auth = 0;
    quint64 timeStamp = 0;
    quint32 length = 0;

    inputStream >> auth;
    inputStream >> timeStamp;
    inputStream >> length;

    if (inputStream.bytesRemaining() != length) {
        qDebug() << "Corrupted packet. Specified length does not equal to real length";
        return;
    }

    QByteArray payload = inputStream.readBytes(length);

    /* Will be implemented later */
    static int pn = 0;

    if (pn == 0)
        answerPqAuthorization(payload);
    else {
    }

    ++pn;
}

void CTelegramCore::initTmpAesKeys()
{
    QByteArray newNonceAndServerNonce;
    newNonceAndServerNonce.append(m_newNonce.data, m_newNonce.size());
    newNonceAndServerNonce.append(m_serverNonce.data, m_serverNonce.size());
    QByteArray serverNonceAndNewNonce;
    serverNonceAndNewNonce.append(m_serverNonce.data, m_serverNonce.size());
    serverNonceAndNewNonce.append(m_newNonce.data, m_newNonce.size());
    QByteArray newNonceAndNewNonce;
    newNonceAndNewNonce.append(m_newNonce.data, m_newNonce.size());
    newNonceAndNewNonce.append(m_newNonce.data, m_newNonce.size());

    m_tmpAesKey = Utils::sha1(newNonceAndServerNonce) + Utils::sha1(serverNonceAndNewNonce).mid(0, 12);
    m_tmpAesIv = Utils::sha1(serverNonceAndNewNonce).mid(12, 8) + Utils::sha1(newNonceAndNewNonce) + QByteArray(m_newNonce.data, 4);
}

void CTelegramCore::sendPackage(const QByteArray &buffer)
{
    QBuffer output;
    output.open(QIODevice::WriteOnly);
    CTelegramStream outputStream(&output);

    outputStream << quint64(0); // Zero auth for initial messages
    outputStream << formatClientTimeStamp(QDateTime::currentMSecsSinceEpoch());
    outputStream << quint32(buffer.length());

    m_transport->sendPackage(output.buffer() + buffer);
}
