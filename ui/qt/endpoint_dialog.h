/** @file
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ENDPOINT_DIALOG_H
#define ENDPOINT_DIALOG_H

#include <QFile>

#include "traffic_table_dialog.h"

class EndpointTreeWidget : public TrafficTableTreeWidget
{
    Q_OBJECT
public:
    explicit EndpointTreeWidget(QWidget *parent, register_ct_t* table);
    ~EndpointTreeWidget();

#ifdef HAVE_MAXMINDDB
    bool hasGeoIPData() const { return has_geoip_data_; }
#endif

    static void tapReset(void *conv_hash_ptr);
    static void tapDraw(void *conv_hash_ptr);

#ifdef HAVE_MAXMINDDB
signals:
    void geoIPStatusChanged();
#endif

private:
    void updateItems();

    QString columnTitle(int nr);

#ifdef HAVE_MAXMINDDB
    bool has_geoip_data_;
#endif
    address_type table_address_type_;

private slots:
    void filterActionTriggered();
};

class EndpointDialog : public TrafficTableDialog
{
    Q_OBJECT
public:
    /** Create a new endpoint window.
     *
     * @param parent Parent widget.
     * @param cf Capture file. No statistics will be calculated if this is NULL.
     * @param cli_proto_id If valid, add this protocol and bring it to the front.
     * @param filter Display filter to apply.
     */
    explicit EndpointDialog(QWidget &parent, CaptureFile &cf, int cli_proto_id = -1, const char *filter = NULL);
    ~EndpointDialog();

signals:

protected:
    void captureFileClosing();
    void captureFileClosed();

private:
#ifdef HAVE_MAXMINDDB
    QPushButton *map_bt_;

    QUrl createMap(bool json_only);
    bool writeEndpointGeoipMap(QFile * fp, bool json_only, hostlist_talker_t *const *hosts);
#endif
    bool addTrafficTable(register_ct_t* table);

private slots:
#ifdef HAVE_MAXMINDDB
    void tabChanged();
    void openMap();
    void saveMap();
#endif
    void on_buttonBox_helpRequested();
};

void init_endpoint_table(struct register_ct* ct, const char *filter);

#endif // ENDPOINT_DIALOG_H
