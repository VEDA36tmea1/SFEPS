#include "recordinglistmodel.h"

RecordingListModel::RecordingListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int RecordingListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) return 0;
    return m_items.count();
}

QVariant RecordingListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return QVariant();
    const RecordingEntry &e = m_items.at(index.row());
    switch (role) {
    case IdRole: return e.id;
    case CreatedAtRole: return e.createdAt;
    default: return QVariant();
    }
}

QHash<int, QByteArray> RecordingListModel::roleNames() const
{
    QHash<int, QByteArray> rn;
    rn[IdRole] = "id";
    rn[CreatedAtRole] = "createdAt";
    return rn;
}

void RecordingListModel::clear()
{
    beginResetModel();
    m_items.clear();
    endResetModel();
    emit countChanged();
}

void RecordingListModel::upsert(const QString &id, const QString &createdAt)
{
    for (int i = 0; i < m_items.count(); ++i) {
        if (m_items[i].id == id) {
            if (m_items[i].createdAt != createdAt) {
                m_items[i].createdAt = createdAt;
                const QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, {CreatedAtRole});
            }
            return;
        }
    }

    beginInsertRows(QModelIndex(), m_items.count(), m_items.count());
    RecordingEntry e;
    e.id = id;
    e.createdAt = createdAt;
    m_items.append(e);
    endInsertRows();
    emit countChanged();
}

void RecordingListModel::removeById(const QString &id)
{
    for (int i = 0; i < m_items.count(); ++i) {
        if (m_items[i].id == id) {
            beginRemoveRows(QModelIndex(), i, i);
            m_items.removeAt(i);
            endRemoveRows();
            emit countChanged();
            return;
        }
    }
}
