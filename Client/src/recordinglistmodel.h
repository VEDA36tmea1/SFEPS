#ifndef RECORDINGLISTMODEL_H
#define RECORDINGLISTMODEL_H

#include <QAbstractListModel>

struct RecordingEntry {
    QString id;
    QString createdAt;
};

class RecordingListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { IdRole = Qt::UserRole + 1, CreatedAtRole };
    explicit RecordingListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_items.count(); }

    Q_INVOKABLE void clear();
    Q_INVOKABLE void upsert(const QString &id, const QString &createdAt);
    Q_INVOKABLE void removeById(const QString &id);

signals:
    void countChanged();

private:
    QList<RecordingEntry> m_items;
};

#endif // RECORDINGLISTMODEL_H
