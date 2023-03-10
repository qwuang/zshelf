#include "store.h"

bool initialInfo = true;
Worker *infoThread = nullptr;

Store::Store() : rootView(rootObject()), context(rootContext())
{
    worker = new Worker({}, true);
    // Start server
    worker->checkServer();
    context->setContextProperty("storeProg", QVariant(0.2));

    connect(worker, &Worker::updateStatus, this, [this](QString stat) {
        qDebug() << "LOG: " << stat;
        if (stat.startsWith("TOTAL:"))
        {
            // Hacky trick to force redrawing Repeater
            QStringList pagesList;
            setProperty("pages", QVariant::fromValue(pagesList));

            int totalItems = stat.mid(6).trimmed().toInt();
            if (totalItems < 50)
                return;

            for (int i = 50; i < totalItems; i += 50)
                pagesList.push_back(QString::number(i));

            pagesList.push_back(QString::number(totalItems));
            setProperty("pages", QVariant::fromValue(pagesList));
        }
    });
    connect(worker, &Worker::updateProgress, this, [this](int prog) {
        context->setContextProperty("storeProg", QVariant(0.2 + prog / 100.0 * 0.7));
    });
    connect(worker, &Worker::socketClosed, this, [this]() {
        context->setContextProperty("titleVisible", QVariant(false));
        setProperty("isBusy", false);
        if (initialInfo && infoThread != nullptr) {
            infoThread->work();
            initialInfo = false;
        }
    });
    connect(worker, &Worker::readAll, this, [this](QByteArray bytes) {
        if (booksParent != nullptr) {
            delete booksParent;
            booksParent = nullptr;
        }

        _books.clear();
        emit booksChanged();

        context->setContextProperty("storeProg", QVariant(0.95));
        QJsonParseError jsonError;
        QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError)
        {
            qDebug() << "fromJson failed: " << jsonError.errorString();
            context->setContextProperty("storeError", bytes);
            return;
        }

        if (!document.isArray())
        {
            context->setContextProperty("storeError", QVariant("ERR: Server response malformed"));
            return;
        }

        // QList<QObject *> booksList;
        QJsonArray list = document.array();
        if (list.size() == 0)
        {
            context->setContextProperty("storeError", QVariant("No result found"));
            return;
        }

        booksParent = new QObject();

        for (auto book : list)
        {
            if (!book.isObject())
                continue;

            QJsonObject bookObj = book.toObject();
            Book *item = new Book(booksParent);
            item->_name = bookObj.value("name").toString();
            item->_author = bookObj.value("author").toString();
            item->_imgFile = bookObj.value("img").toString();
            if (item->_imgFile.size() > 0) {
                item->_imgFile.prepend("image://gray/");
            }
            item->_url = bookObj.value("url").toString();

            _books.push_back(item);
        }

        emit booksChanged();
        context->setContextProperty("storeProg", QVariant(1));
        context->setContextProperty("storeError", QVariant(""));
    });
}

Store::~Store()
{
    if (worker != nullptr)
        delete worker;

    if (infoThread != nullptr)
        delete infoThread;
    
    if (serverProc != nullptr)
        delete serverProc;
}

void Store::open()
{
    if (!loadConfig())
        qDebug() << "config.json malformed";

    newQuery(0);

    if (_cookieAvailable)
    {
        infoThread = new Worker({"INFO"}, true);
        connect(infoThread, &Worker::readAll, this, [this](QByteArray bytes) {
            QJsonParseError jsonError;
            QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
            if (jsonError.error != QJsonParseError::NoError)
            {
                qDebug() << "fromJson failed: " << jsonError.errorString();
                qDebug() << "ERR: " << bytes;
                return;
            }
            if (!document.isObject())
                return;

            QJsonObject jsonObj = document.object();
            QString downloads = jsonObj.value("today_download").toString("");
            if (downloads.length() > 0)
            {
                auto counts = downloads.split("/");
                downloads.prepend("??? ");
                if (counts[0] == counts[1])
                {
                    downloads.prepend("?????? ");
                }
                this->setProperty("accountStatus", downloads);
            }

            QJsonValue historyList = jsonObj.value("today_list");
            if (!historyList.isArray())
                return;
            QJsonArray downloadedBooks = historyList.toArray();
            for (auto book : downloadedBooks)
            {
                auto bookObj = book.toObject();
                QString url = bookObj.value("url").toString();

                bool found = false;
                for (auto seen : _downloadList)
                {
                    if (url == seen->property("url"))
                    {
                        found = true;
                        break;
                    }
                }
                if (found)
                    continue;
                Book *item = new Book(nullptr);
                item->_url = url;
                item->_name = bookObj.value("name").toString();
                _downloadList.push_back(item);
            }
            emit downloadListChanged();
        });
    }
    else
    {
        setProperty("accountStatus", "?????? Cookie is not configured");
    }
}

void Store::newQuery(int page = 0)
{
    _currentPage = page;
    QStringList args = {
        "LIST",
        _exactMatch,
        _fromYear,
        _toYear,
        _language,
        _extension,
        _order,
        _query,
        QString::number(page + 1)
    };

    stopQuery();
    setProperty("isBusy", true);
    worker->args = args;
    context->setContextProperty("storeProg", QVariant(0.2));
    worker->work();
}

void Store::openSavedList(int page = 0)
{
    _currentPage = page;
    QStringList args = {
        "SAVE",
        QString::number(page + 1)
    };

    stopQuery();
    setProperty("isBusy", true);
    worker->args = args;
    context->setContextProperty("storeProg", QVariant(0.2));
    worker->work();
}

void Store::stopQuery()
{
    if (worker->isRunning())
        worker->terminate();
    setProperty("isBusy", false);
}

bool Store::loadConfig()
{
    QFile file(QCoreApplication::applicationDirPath() + "/config.json");

    if (!file.open(QIODevice::ReadOnly))
        return false;

    QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError jsonError;
    QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
    if (jsonError.error != QJsonParseError::NoError)
    {
        qDebug() << "fromJson failed: " << jsonError.errorString();
        return false;
    }
    if (!document.isObject())
        return false;

    QJsonObject jsonObj = document.object();

    _cookieAvailable = jsonObj.value("cookie").toString("").length() > 0;

    QJsonValue defaultQueryValue = jsonObj.value("defaultQuery");
    if (!defaultQueryValue.isObject())
        return false;

    QJsonObject defaultQueryObj = defaultQueryValue.toObject();

    _exactMatch = defaultQueryObj.value("exactMatch").toString("");
    _fromYear = defaultQueryObj.value("fromYear").toString("");
    _toYear = defaultQueryObj.value("toYear").toString("");
    _language = defaultQueryObj.value("language").toString("");
    _extension = defaultQueryObj.value("extension").toString("");
    _order = defaultQueryObj.value("order").toString("");
    _query = defaultQueryObj.value("query").toString("");

    return true;
}

bool Store::setConfig()
{
    QFile file(QCoreApplication::applicationDirPath() + "/config.json");

    if (!file.open(QIODevice::ReadOnly))
    {
        qDebug() << "Can't open config.json in read-only";
        return false;
    }

    QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError jsonError;
    QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
    if (jsonError.error != QJsonParseError::NoError)
    {
        qDebug() << "fromJson failed: " << jsonError.errorString();
        return false;
    }
    if (!document.isObject())
    {
        qDebug() << "config.json malformed";
        return false;
    }

    QJsonObject jsonObj = document.object();

    QJsonObject defaultQuery;

    defaultQuery.insert("exactMatch", _exactMatch);
    defaultQuery.insert("fromYear", _fromYear);
    defaultQuery.insert("toYear", _toYear);
    defaultQuery.insert("language", _language);
    defaultQuery.insert("extension", _extension);
    defaultQuery.insert("order", _order);
    defaultQuery.insert("query", _query);

    jsonObj.remove("defaultQuery");
    jsonObj.insert("defaultQuery", defaultQuery);

    file.remove();
    if (!file.open(QIODevice::WriteOnly))
    {
        qDebug() << "Can't open config.json in write-only";
        return false;
    }

    QByteArray writeBytes = QJsonDocument(jsonObj).toJson(QJsonDocument::Indented);

    QTextStream iStream(&file);
    iStream.setCodec("utf-8");
    iStream << writeBytes;
    file.close();

    return true;
}

void Store::download(Book* book)
{
    if (book->worker == nullptr) {
        book->worker = new Worker({"DOWN", book->_dlUrl});
        connect(book->worker, &Worker::updateProgress, book, &Book::updateProgress);
        connect(book->worker, &Worker::updateStatus, book, [book](QString stat) {
            qDebug() << "LOG: " << stat;
            if (stat.startsWith("ERR:"))
            {
                book->setProperty("status", QVariant(stat.trimmed()));
                book->worker->terminate();
                delete book->worker;
            }
        });
        connect(book->worker, &Worker::socketClosed, book, [book]() {
            if (infoThread != nullptr && !infoThread->isRunning())
            {
                infoThread->work();
            }
        });
    }

    book->setParent(nullptr);
    _downloadList.prepend(book);
    emit downloadListChanged();

    book->setProperty("status", QVariant("Downloading"));
    book->worker->work();
}

Book::~Book() {
    if (worker != nullptr) {
        delete worker;
    }
}

void Book::getDetail(QObject* popup)
{
    if (_metadownloaded)
    {
        return;
    }

    Worker *metaWorker = new Worker({"META", _url}, true);
    connect(metaWorker, &Worker::readAll, this, [this, metaWorker, popup](QByteArray bytes) {
        QJsonParseError jsonError;
        QJsonDocument document = QJsonDocument::fromJson(bytes, &jsonError);
        if (jsonError.error != QJsonParseError::NoError)
        {
            qDebug() << "fromJson failed: " << jsonError.errorString();
            setProperty("desc", bytes);
            return;
        }
        if (!document.isObject())
        {
            return;
        }
        QJsonObject detail = document.object();
        setProperty("name", detail.value("name").toString());
        setProperty("author", detail.value("author").toString());
        setProperty("dlUrl", detail.value("dlUrl").toString());
        setProperty("desc", detail.value("description").toString());
        auto imgFile = detail.value("img").toString();
        if (imgFile.size() > 0)
        {
            imgFile.prepend("image://gray/");
        }
        setProperty("imgFile", imgFile);

        QJsonArray similarsArray = detail.value("similars").toArray();
        QList<QObject *> recList;
        for (auto recom : similarsArray)
        {
            QJsonObject bookObj = recom.toObject();
            Book *item = new Book(this);
            item->setProperty("imgFile", bookObj.value("img").toString());
            item->setProperty("url", bookObj.value("url").toString());
            recList.push_back(item);
        }
        setProperty("similars", QVariant::fromValue(recList));

        setProperty("status", "Download");
        _metadownloaded = true;
        popup->setProperty("isBusy", false);

        qDebug() << "Meta downloaded";
        delete metaWorker;
    });

    qDebug() << "Meta downloading";
    popup->setProperty("isBusy", true);
    metaWorker->work();
}

void Book::updateProgress(int prog)
{
    if (prog == 100)
    {
        setProperty("status", QVariant("Downloaded"));
        return;
    }
    setProperty("status", QVariant(QString::number(prog) + "%"));
}