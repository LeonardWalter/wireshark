/* endpoint_dialog.cpp
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "endpoint_dialog.h"

#include <epan/maxmind_db.h>

#include <epan/prefs.h>
#include <epan/to_str.h>

#include "ui/recent.h"

#include "wsutil/filesystem.h"
#include "wsutil/file_util.h"
#include "wsutil/pint.h"
#include "wsutil/str_util.h"
#include <wsutil/utf8_entities.h>

#include <ui/qt/utils/qt_ui_utils.h>
#include <ui/qt/utils/variant_pointer.h>
#include <ui/qt/widgets/wireshark_file_dialog.h>
#include "main_application.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QTemporaryFile>

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>

typedef enum
{
    ENDP_COLUMN_ADDR,
    ENDP_COLUMN_PORT,
    ENDP_COLUMN_PACKETS,
    ENDP_COLUMN_BYTES,
    ENDP_COLUMN_PKT_AB,
    ENDP_COLUMN_BYTES_AB,
    ENDP_COLUMN_PKT_BA,
    ENDP_COLUMN_BYTES_BA,
    ENDP_NUM_COLUMNS,
    ENDP_COLUMN_GEO_COUNTRY = ENDP_NUM_COLUMNS,
    ENDP_COLUMN_GEO_CITY,
    ENDP_COLUMN_GEO_AS_NUM,
    ENDP_COLUMN_GEO_AS_ORG,
    ENDP_NUM_GEO_COLUMNS
} endpoint_column_type_e;

static const QString table_name_ = QObject::tr("Endpoint");
EndpointDialog::EndpointDialog(QWidget &parent, CaptureFile &cf, int cli_proto_id, const char *filter) :
    TrafficTableDialog(parent, cf, filter, table_name_)
{
#ifdef HAVE_MAXMINDDB
    map_bt_ = buttonBox()->addButton(tr("Map"), QDialogButtonBox::ActionRole);
    map_bt_->setToolTip(tr("Draw IPv4 or IPv6 endpoints on a map."));
    connect(trafficTableTabWidget(), &QTabWidget::currentChanged, this, &EndpointDialog::tabChanged);

    QMenu *map_menu_ = new QMenu(map_bt_);
    QAction *action;
    action = map_menu_->addAction(tr("Open in browser"));
    connect(action, &QAction::triggered, this, &EndpointDialog::openMap);
    action = map_menu_->addAction(tr("Save As…"));
    connect(action, &QAction::triggered, this, &EndpointDialog::saveMap);
    map_bt_->setMenu(map_menu_);
#endif
    addProgressFrame(&parent);

    QList<int> endp_protos;
    for (GList *endp_tab = recent.endpoint_tabs; endp_tab; endp_tab = endp_tab->next) {
        int proto_id = proto_get_id_by_short_name((const char *)endp_tab->data);
        if (proto_id > -1 && !endp_protos.contains(proto_id)) {
            endp_protos.append(proto_id);
        }
    }

    if (endp_protos.isEmpty()) {
        endp_protos = defaultProtos();
    }

    // Bring the command-line specified type to the front.
    if ((cli_proto_id > 0) && (get_conversation_by_proto_id(cli_proto_id))) {
        endp_protos.removeAll(cli_proto_id);
        endp_protos.prepend(cli_proto_id);
    }

    // QTabWidget selects the first item by default.
    foreach (int endp_proto, endp_protos) {
        addTrafficTable(get_conversation_by_proto_id(endp_proto));
    }

    fillTypeMenu(endp_protos);

    QPushButton *close_bt = buttonBox()->button(QDialogButtonBox::Close);
    if (close_bt) {
        close_bt->setDefault(true);
    }

    updateWidgets();
//    currentTabChanged();

    cap_file_.delayedRetapPackets();
}

EndpointDialog::~EndpointDialog()
{
    prefs_clear_string_list(recent.endpoint_tabs);
    recent.endpoint_tabs = NULL;

    EndpointTreeWidget *cur_tree = qobject_cast<EndpointTreeWidget *>(trafficTableTabWidget()->currentWidget());
    foreach (QAction *ea, traffic_type_menu_.actions()) {
        int proto_id = ea->data().value<int>();
        if (proto_id_to_tree_.contains(proto_id) && ea->isChecked()) {
            char *title = g_strdup(proto_get_protocol_short_name(find_protocol_by_id(proto_id)));
            if (proto_id_to_tree_[proto_id] == cur_tree) {
                recent.endpoint_tabs = g_list_prepend(recent.endpoint_tabs, title);
            } else {
                recent.endpoint_tabs = g_list_append(recent.endpoint_tabs, title);
            }
        }
    }
}

void EndpointDialog::captureFileClosing()
{
    // Keep the dialog around but disable any controls that depend
    // on a live capture file.
    for (int i = 0; i < trafficTableTabWidget()->count(); i++) {
        EndpointTreeWidget *cur_tree = qobject_cast<EndpointTreeWidget *>(trafficTableTabWidget()->widget(i));
        disconnect(cur_tree, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)),
                   this, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)));
    }
    TrafficTableDialog::captureFileClosing();
}

void EndpointDialog::captureFileClosed()
{
    displayFilterCheckBox()->setEnabled(false);
    enabledTypesPushButton()->setEnabled(false);
    TrafficTableDialog::captureFileClosed();
}

bool EndpointDialog::addTrafficTable(register_ct_t *table)
{
    int proto_id = get_conversation_proto_id(table);

    if (!table || proto_id_to_tree_.contains(proto_id)) {
        return false;
    }

    EndpointTreeWidget *endp_tree = new EndpointTreeWidget(this, table);

    proto_id_to_tree_[proto_id] = endp_tree;
    const char* table_name = proto_get_protocol_short_name(find_protocol_by_id(proto_id));

    trafficTableTabWidget()->addTab(endp_tree, table_name);

    connect(endp_tree, SIGNAL(titleChanged(QWidget*,QString)),
            this, SLOT(setTabText(QWidget*,QString)));
    connect(endp_tree, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)),
            this, SIGNAL(filterAction(QString,FilterAction::Action,FilterAction::ActionType)));
    connect(nameResolutionCheckBox(), SIGNAL(toggled(bool)),
            endp_tree, SLOT(setNameResolutionEnabled(bool)));
#ifdef HAVE_MAXMINDDB
    connect(endp_tree, &EndpointTreeWidget::geoIPStatusChanged,
            this, &EndpointDialog::tabChanged);
#endif

    // XXX Move to ConversationTreeWidget ctor?
    QByteArray filter_utf8;
    const char *filter = NULL;
    if (displayFilterCheckBox()->isChecked()) {
        filter = cap_file_.capFile()->dfilter;
    } else if (!filter_.isEmpty()) {
        filter_utf8 = filter_.toUtf8();
        filter = filter_utf8.constData();
    }

    endp_tree->trafficTreeHash()->user_data = endp_tree;

    registerTapListener(proto_get_protocol_filter_name(proto_id), endp_tree->trafficTreeHash(), filter, 0,
                        EndpointTreeWidget::tapReset,
                        get_hostlist_packet_func(table),
                        EndpointTreeWidget::tapDraw);
    return true;
}

#ifdef HAVE_MAXMINDDB
void EndpointDialog::tabChanged()
{
    EndpointTreeWidget *cur_tree = qobject_cast<EndpointTreeWidget *>(trafficTableTabWidget()->currentWidget());
    map_bt_->setEnabled(cur_tree && cur_tree->hasGeoIPData());
}

bool
EndpointDialog::writeEndpointGeoipMap(QFile * fp, bool json_only, hostlist_talker_t *const *hosts)
{
    QTextStream out(fp);

    if (!json_only) {
        QFile ipmap(get_datafile_path("ipmap.html"));

        if (!ipmap.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, tr("Map file error"), tr("Could not open base file %1 for reading: %2")
                .arg(get_datafile_path("ipmap.html"))
                .arg(g_strerror(errno))
            );
            return false;
        }

        /* Copy ipmap.html to map file. */
        QTextStream in(&ipmap);
        QString line;
        while (in.readLineInto(&line)) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            out << line << Qt::endl;
#else
            out << line << endl;
#endif            
        }

        out << QString("<script id=\"ipmap-data\" type=\"application/json\">\n");
    }

    /*
     * Writes a feature for each resolved address, the output will look like:
     *  {
     *    "type": "FeatureCollection",
     *    "features": [
     *      {
     *        "type": "Feature",
     *        "geometry": {
     *          "type": "Point",
     *          "coordinates": [ -97.821999, 37.750999 ]
     *        },
     *        "properties": {
     *          "ip": "8.8.4.4",
     *          "autonomous_system_number": 15169,
     *          "autonomous_system_organization": "Google LLC",
     *          "city": "(omitted, but key is shown for documentation reasons)",
     *          "country": "United States",
     *          "radius": 1000,
     *          "packets": 1,
     *          "bytes": 1543
     *        }
     *      }
     *    ]
     *  }
     */

    QJsonObject root;
    root["type"] = "FeatureCollection";
    QJsonArray features;

    /* Append map data. */
    size_t count = 0;
    const hostlist_talker_t *host;
    for (hostlist_talker_t *const *iter = hosts; (host = *iter) != NULL; ++iter) {
        QJsonObject arrEntry;

        char addr[WS_INET6_ADDRSTRLEN];
        const mmdb_lookup_t *result = NULL;
        if (host->myaddress.type == AT_IPv4) {
            const ws_in4_addr *ip4 = (const ws_in4_addr *)host->myaddress.data;
            result = maxmind_db_lookup_ipv4(ip4);
            ws_inet_ntop4(ip4, addr, sizeof(addr));
        } else if (host->myaddress.type == AT_IPv6) {
            const ws_in6_addr *ip6 = (const ws_in6_addr *)host->myaddress.data;
            result = maxmind_db_lookup_ipv6(ip6);
            ws_inet_ntop6(ip6, addr, sizeof(addr));
        }
        if (!maxmind_db_has_coords(result)) {
            // result could be NULL if the caller did not trigger a lookup
            // before. result->found could be FALSE if no MMDB entry exists.
            continue;
        }

        ++count;
        arrEntry["type"] = "Feature";
        QJsonObject geometry;
        geometry["type"] = "Point";
        QJsonArray coordinates;
        coordinates.append(QJsonValue(result->longitude));
        coordinates.append(QJsonValue(result->latitude));
        geometry["coordinates"] = coordinates;
        arrEntry["geometry"] = geometry;

        QJsonObject property;
        property["ip"] = addr;
        if (result->as_number && result->as_org) {
            property["autonomous_system_number"] = QJsonValue((int)(result->as_number));
            property["autonomous_system_organization"] = QJsonValue(result->as_org);
        }

        if (result->city)
            property["city"] = result->city;
        if (result->country)
            property["country"] = result->country;
        if (result->accuracy)
            property["radius"] = QJsonValue(result->accuracy);

        property["packets"] = QJsonValue((qint64)(host->rx_frames + host->tx_frames));
        property["bytes"] = QJsonValue((qint64)(host->rx_bytes + host->tx_bytes));
        arrEntry["properties"] = property;
        features.append(arrEntry);
    }
    root["features"] = features;
    QJsonDocument doc;
    doc.setObject(root);

    out << doc.toJson();

    if (!json_only) 
        out << QString("</script>\n");

    if (count == 0) {
        QMessageBox::warning(this, tr("Map file error"), tr("No endpoints available to map"));
        return false;
    }

    out.flush();

    return true;
}

QUrl EndpointDialog::createMap(bool json_only)
{
    EndpointTreeWidget *cur_tree = qobject_cast<EndpointTreeWidget *>(trafficTableTabWidget()->currentWidget());
    if (!cur_tree) {
        return QUrl();
    }

    // Construct list of hosts with a valid MMDB entry.
    QTreeWidgetItemIterator it(cur_tree);
    GPtrArray *hosts_arr = g_ptr_array_new();
    while (*it) {
        const mmdb_lookup_t *geo = VariantPointer<const mmdb_lookup_t>::asPtr((*it)->data(0, Qt::UserRole + 1));
        if (maxmind_db_has_coords(geo)) {
            hostlist_talker_t *host = VariantPointer<hostlist_talker_t>::asPtr((*it)->data(0, Qt::UserRole));
            g_ptr_array_add(hosts_arr, (gpointer)host);
        }
        ++it;
    }
    if (hosts_arr->len == 0) {
        QMessageBox::warning(this, tr("Map file error"), tr("No endpoints available to map"));
        g_ptr_array_free(hosts_arr, TRUE);
        return QUrl();
    }
    g_ptr_array_add(hosts_arr, NULL);
    hostlist_talker_t **hosts = (hostlist_talker_t **)g_ptr_array_free(hosts_arr, FALSE);

    QString tempname = QString("%1/ipmapXXXXXX.html").arg(QDir::tempPath());
    QTemporaryFile tf(tempname);
    if (!tf.open()) {
        QMessageBox::warning(this, tr("Map file error"), tr("Unable to create temporary file"));
        g_free(hosts);
        return QUrl();
    }

    if (!writeEndpointGeoipMap(&tf, json_only, hosts)) {
        g_free(hosts);
        tf.close();
        return QUrl();
    }
    g_free(hosts);

    tf.setAutoRemove(false);
    return QUrl::fromLocalFile(tf.fileName());
}

void EndpointDialog::openMap()
{
    QUrl map_file = createMap(false);
    if (!map_file.isEmpty()) {
        QDesktopServices::openUrl(map_file);
    }
}

void EndpointDialog::saveMap()
{
    QString destination_file =
        WiresharkFileDialog::getSaveFileName(this, tr("Save Endpoints Map"),
                "ipmap.html",
                "HTML files (*.html);;GeoJSON files (*.json)");
    if (destination_file.isEmpty()) {
        return;
    }
    QUrl map_file = createMap(destination_file.endsWith(".json"));
    if (!map_file.isEmpty()) {
        QString source_file = map_file.toLocalFile();
        QFile::remove(destination_file);
        if (!QFile::rename(source_file, destination_file)) {
            QMessageBox::warning(this, tr("Map file error"),
                    tr("Failed to save map file %1.").arg(destination_file));
            QFile::remove(source_file);
        }
    }
}
#endif

void EndpointDialog::on_buttonBox_helpRequested()
{
    mainApp->helpTopicAction(HELP_STATS_ENDPOINTS_DIALOG);
}

void init_endpoint_table(struct register_ct* ct, const char *filter)
{
    mainApp->emitStatCommandSignal("Endpoints", filter, GINT_TO_POINTER(get_conversation_proto_id(ct)));
}

// EndpointTreeWidgetItem
// TrafficTableTreeWidgetItem / QTreeWidgetItem subclass that allows sorting

static const char *data_none_ = UTF8_EM_DASH;

class EndpointTreeWidgetItem : public TrafficTableTreeWidgetItem
{
public:
    EndpointTreeWidgetItem(GArray *conv_array, guint conv_idx, bool *resolve_names_ptr) :
        TrafficTableTreeWidgetItem(NULL),
        conv_array_(conv_array),
        conv_idx_(conv_idx),
        resolve_names_ptr_(resolve_names_ptr)
    {}

    hostlist_talker_t *hostlistTalker() {
        return &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);
    }

    virtual QVariant data(int column, int role) const {
        if (role == Qt::DisplayRole) {
            // Column text cooked representation.
            hostlist_talker_t *endp_item = &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);

            bool resolve_names = false;
            if (resolve_names_ptr_ && *resolve_names_ptr_) resolve_names = true;
            switch (column) {
            case ENDP_COLUMN_PACKETS:
                return QString("%L1").arg(endp_item->tx_frames + endp_item->rx_frames);
            case ENDP_COLUMN_BYTES:
                return gchar_free_to_qstring(format_size(endp_item->tx_bytes + endp_item->rx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            case ENDP_COLUMN_PKT_AB:
                return QString("%L1").arg(endp_item->tx_frames);
            case ENDP_COLUMN_BYTES_AB:
                return gchar_free_to_qstring(format_size(endp_item->tx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            case ENDP_COLUMN_PKT_BA:
                return QString("%L1").arg(endp_item->rx_frames);
            case ENDP_COLUMN_BYTES_BA:
                return gchar_free_to_qstring(format_size(endp_item->rx_bytes, FORMAT_SIZE_UNIT_NONE, FORMAT_SIZE_PREFIX_SI));
            default:
                QVariant col_data = colData(column, resolve_names);
                if (col_data.isValid()) return col_data;
                return QVariant(data_none_);
            }
        }
        if (role == Qt::UserRole) {
            hostlist_talker_t *endp_item = &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);
            return VariantPointer<hostlist_talker_t>::asQVariant(endp_item);
        }
        if (role == Qt::UserRole + 1) {
            return VariantPointer<const mmdb_lookup_t>::asQVariant(mmdbLookup());
        }
        return QTreeWidgetItem::data(column, role);
    }

    const mmdb_lookup_t *mmdbLookup() const {
        hostlist_talker_t *endp_item = &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);
        const mmdb_lookup_t *mmdb_lookup = NULL;
        if (endp_item->myaddress.type == AT_IPv4) {
            mmdb_lookup = maxmind_db_lookup_ipv4((const ws_in4_addr *) endp_item->myaddress.data);
        } else if (endp_item->myaddress.type == AT_IPv6) {
            mmdb_lookup = maxmind_db_lookup_ipv6((const ws_in6_addr *) endp_item->myaddress.data);
        }
        return mmdb_lookup && mmdb_lookup->found ? mmdb_lookup : NULL;
    }

    // Column text raw representation.
    // Return a string, qulonglong, double, or invalid QVariant representing the raw column data.
    QVariant colData(int col, bool resolve_names) const {
        hostlist_talker_t *endp_item = &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);
        const mmdb_lookup_t *mmdb_lookup = mmdbLookup();

        switch (col) {
        case ENDP_COLUMN_ADDR:
        {
            char* addr_str = get_conversation_address(NULL, &endp_item->myaddress, resolve_names);
            QString q_addr_str(addr_str);
            wmem_free(NULL, addr_str);
            return q_addr_str;
        }
        case ENDP_COLUMN_PORT:
            if (resolve_names) {
                char* port_str = get_conversation_port(NULL, endp_item->port, endp_item->etype, resolve_names);
                QString q_port_str(port_str);
                wmem_free(NULL, port_str);
                return q_port_str;
            } else {
                return quint32(endp_item->port);
            }
        case ENDP_COLUMN_PACKETS:
            return quint64(endp_item->tx_frames + endp_item->rx_frames);
        case ENDP_COLUMN_BYTES:
            return quint64(endp_item->tx_bytes + endp_item->rx_bytes);
        case ENDP_COLUMN_PKT_AB:
            return quint64(endp_item->tx_frames);
        case ENDP_COLUMN_BYTES_AB:
            return quint64(endp_item->tx_bytes);
        case ENDP_COLUMN_PKT_BA:
            return quint64(endp_item->rx_frames);
        case ENDP_COLUMN_BYTES_BA:
            return quint64(endp_item->rx_bytes);
        case ENDP_COLUMN_GEO_COUNTRY:
            if (mmdb_lookup && mmdb_lookup->country) {
                return QVariant(mmdb_lookup->country);
            }
            return QVariant();
        case ENDP_COLUMN_GEO_CITY:
            if (mmdb_lookup && mmdb_lookup->city) {
                return QVariant(mmdb_lookup->city);
            }
            return QVariant();
        case ENDP_COLUMN_GEO_AS_NUM:
            if (mmdb_lookup && mmdb_lookup->as_number) {
                return QVariant(mmdb_lookup->as_number);
            }
            return QVariant();
        case ENDP_COLUMN_GEO_AS_ORG:
            if (mmdb_lookup && mmdb_lookup->as_org) {
                return QVariant(mmdb_lookup->as_org);
            }
            return QVariant();

        default:
            return QVariant();
        }
    }

    bool operator< (const QTreeWidgetItem &other) const
    {
        const EndpointTreeWidgetItem *other_row = static_cast<const EndpointTreeWidgetItem *>(&other);
        hostlist_talker_t *endp_item = &g_array_index(conv_array_, hostlist_talker_t, conv_idx_);
        hostlist_talker_t *other_item = &g_array_index(other_row->conv_array_, hostlist_talker_t, other_row->conv_idx_);

        bool resolve_names = false;
        if (resolve_names_ptr_ && *resolve_names_ptr_) resolve_names = true;

        int sort_col = treeWidget()->sortColumn();

        switch(sort_col) {
        case ENDP_COLUMN_ADDR:
            if (resolve_names) {
                char* addr_str = address_to_display(NULL, &endp_item->myaddress);
                char* otheraddr_str = address_to_display(NULL, &other_item->myaddress);
                bool ret;

                ret = g_ascii_strcasecmp(addr_str, otheraddr_str) < 0 ? true : false;
                wmem_free(NULL, otheraddr_str);
                wmem_free(NULL, addr_str);
                return ret;
	    } else {
                return cmp_address(&endp_item->myaddress, &other_item->myaddress) < 0 ? true : false;
            }
        case ENDP_COLUMN_PORT:
            return endp_item->port < other_item->port;
        case ENDP_COLUMN_PACKETS:
            return (endp_item->tx_frames + endp_item->rx_frames) < (other_item->tx_frames + other_item->rx_frames);
        case ENDP_COLUMN_BYTES:
            return (endp_item->tx_bytes + endp_item->rx_bytes) < (other_item->tx_bytes + other_item->rx_bytes);
        case ENDP_COLUMN_PKT_AB:
            return endp_item->tx_frames < other_item->tx_frames;
        case ENDP_COLUMN_BYTES_AB:
            return endp_item->tx_bytes < other_item->tx_bytes;
        case ENDP_COLUMN_PKT_BA:
            return endp_item->rx_frames < other_item->rx_frames;
        case ENDP_COLUMN_BYTES_BA:
            return endp_item->rx_bytes < other_item->rx_bytes;
        case ENDP_COLUMN_GEO_COUNTRY:
        case ENDP_COLUMN_GEO_CITY:
        case ENDP_COLUMN_GEO_AS_ORG:
        {
            QString this_str = data(sort_col, Qt::DisplayRole).toString();
            QString other_str = other_row->data(sort_col, Qt::DisplayRole).toString();
            return (this_str < other_str);
        }
        case ENDP_COLUMN_GEO_AS_NUM:
        {
            // Valid values first, similar to strings above.
            bool ok;
            unsigned this_asn = colData(sort_col, false).toUInt(&ok);
            if (!ok) this_asn = UINT_MAX;
            unsigned other_asn = other_row->colData(sort_col, false).toUInt(&ok);
            if (!ok) other_asn = UINT_MAX;
            return (this_asn < other_asn);
        }
        default:
            return false;
        }
    }
private:
    GArray *conv_array_;
    guint conv_idx_;
    bool *resolve_names_ptr_;
};

//
// EndpointTreeWidget
// TrafficTableTreeWidget / QTreeWidget subclass that allows tapping
//

EndpointTreeWidget::EndpointTreeWidget(QWidget *parent, register_ct_t *table) :
    TrafficTableTreeWidget(parent, table),
#ifdef HAVE_MAXMINDDB
    has_geoip_data_(false),
#endif
    table_address_type_(AT_NONE)
{
    setColumnCount(ENDP_NUM_COLUMNS);
    setUniformRowHeights(true);

    QString proto_filter_name = proto_get_protocol_filter_name(get_conversation_proto_id(table_));
    if (proto_filter_name == "ip") {
        table_address_type_ = AT_IPv4;
    } else if (proto_filter_name == "ipv6") {
        table_address_type_ = AT_IPv6;
    }
    if (get_conversation_hide_ports(table_)) {
        hideColumn(ENDP_COLUMN_PORT);
    } else if (proto_filter_name == "ncp") {
        headerItem()->setText(ENDP_COLUMN_PORT, tr("Connection"));
    }

    int column_count = ENDP_NUM_COLUMNS;
    if (table_address_type_ == AT_IPv4 || table_address_type_ == AT_IPv6) {
        column_count = ENDP_NUM_GEO_COLUMNS;
    }
    for (int col = 0; col < column_count; col++) {
        headerItem()->setText(col, columnTitle(col));
    }


    int one_en = fontMetrics().height() / 2;
    for (int i = 0; i < columnCount(); i++) {
        switch (i) {
        case ENDP_COLUMN_ADDR:
            setColumnWidth(i, one_en * (int) strlen("000.000.000.000"));
            break;
        case ENDP_COLUMN_PORT:
            setColumnWidth(i, one_en * (int) strlen("000000"));
            break;
        case ENDP_COLUMN_PACKETS:
        case ENDP_COLUMN_PKT_AB:
        case ENDP_COLUMN_PKT_BA:
            setColumnWidth(i, one_en * (int) strlen("00,000"));
            break;
        case ENDP_COLUMN_BYTES:
        case ENDP_COLUMN_BYTES_AB:
        case ENDP_COLUMN_BYTES_BA:
            setColumnWidth(i, one_en * (int) strlen("000,000"));
            break;
        default:
            setColumnWidth(i, one_en * 15); // Geolocation
        }
    }

    QMenu *submenu;

    FilterAction::Action cur_action = FilterAction::ActionApply;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, at);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    cur_action = FilterAction::ActionPrepare;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, at);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    cur_action = FilterAction::ActionFind;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, at);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    cur_action = FilterAction::ActionColorize;
    submenu = ctx_menu_.addMenu(FilterAction::actionName(cur_action));
    foreach (FilterAction::ActionType at, FilterAction::actionTypes()) {
        FilterAction *fa = new FilterAction(submenu, cur_action, at);
        submenu->addAction(fa);
        connect(fa, SIGNAL(triggered()), this, SLOT(filterActionTriggered()));
    }

    ctx_menu_.addSeparator();
    QAction * act = ctx_menu_.addAction(tr("Resize all columns to content"));
    connect(act, &QAction::triggered, [this]() {
        for (int col = 0; col < this->columnCount(); col++)
            this->resizeColumnToContents(col);
    });

    updateItems();

}

EndpointTreeWidget::~EndpointTreeWidget()
{
    reset_hostlist_table_data(&hash_);
}

QString EndpointTreeWidget::columnTitle(int col)
{
    switch (col) {
        case 0:
            return tr("Address"); break;
        case 1:
            return tr("Port"); break;
        case 2:
            return tr("Packets"); break;
        case 3:
            return tr("Bytes"); break;
        case 4:
            return tr("Tx Packets"); break;
        case 5:
            return tr("Tx Bytes"); break;
        case 6:
            return tr("Rx Packets"); break;
        case 7:
            return tr("Rx Bytes"); break;
        case 8:
            return tr("Country"); break;
        case 9:
            return tr("City"); break;
        case 10:
            return tr("AS Number"); break;
        case 11:
            return tr("AS Organization"); break;
    }

    return QString();
}

void EndpointTreeWidget::tapReset(void *conv_hash_ptr)
{
    conv_hash_t *hash = (conv_hash_t*)conv_hash_ptr;
    EndpointTreeWidget *endp_tree = qobject_cast<EndpointTreeWidget *>((EndpointTreeWidget *)hash->user_data);
    if (!endp_tree) return;

    endp_tree->clear();
    reset_hostlist_table_data(&endp_tree->hash_);
}

void EndpointTreeWidget::tapDraw(void *conv_hash_ptr)
{
    conv_hash_t *hash = (conv_hash_t*)conv_hash_ptr;
    EndpointTreeWidget *endp_tree = qobject_cast<EndpointTreeWidget *>((EndpointTreeWidget *)hash->user_data);
    if (!endp_tree) return;

    endp_tree->updateItems();
}

void EndpointTreeWidget::updateItems()
{
    bool resize = topLevelItemCount() < resizeThreshold();
    title_ = proto_get_protocol_short_name(find_protocol_by_id(get_conversation_proto_id(table_)));

    if (hash_.conv_array && hash_.conv_array->len > 0) {
        title_.append(QString(" %1 %2").arg(UTF8_MIDDLE_DOT).arg(hash_.conv_array->len));
    }
    emit titleChanged(this, title_);

    if (!hash_.conv_array) {
        return;
    }

    setSortingEnabled(false);

    QList<QTreeWidgetItem *>new_items;
    for (int i = topLevelItemCount(); i < (int) hash_.conv_array->len; i++) {
        EndpointTreeWidgetItem *etwi = new EndpointTreeWidgetItem(hash_.conv_array, i, &resolve_names_);
        new_items << etwi;

        for (int col = 0; col < columnCount(); col++) {
            if (col != ENDP_COLUMN_ADDR && col < ENDP_NUM_COLUMNS) {
                etwi->setTextAlignment(col, Qt::AlignRight);
            }
        }

#ifdef HAVE_MAXMINDDB
        // Assume that an asynchronous MMDB lookup has completed before (for
        // example, in the dissection tree). If so, then we do not have to check
        // all previous items for availability of any MMDB result.
        if (!has_geoip_data_ && maxmind_db_has_coords(etwi->mmdbLookup())) {
            has_geoip_data_ = true;
            emit geoIPStatusChanged();
        }
#endif
    }
    addTopLevelItems(new_items);
    setSortingEnabled(true);

    if (resize) {
        for (int col = 0; col < columnCount(); col++) {
            resizeColumnToContents(col);
        }
    }
}

void EndpointTreeWidget::filterActionTriggered()
{
    EndpointTreeWidgetItem *etwi = static_cast<EndpointTreeWidgetItem *>(currentItem());
    FilterAction *fa = qobject_cast<FilterAction *>(QObject::sender());

    if (!fa || !etwi) {
        return;
    }

    hostlist_talker_t *endp_item = etwi->hostlistTalker();
    if (!endp_item) {
        return;
    }

    QString filter = get_hostlist_filter(endp_item);
    emit filterAction(filter, fa->action(), fa->actionType());
}
