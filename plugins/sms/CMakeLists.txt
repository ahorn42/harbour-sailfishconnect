kdeconnect_add_plugin(
    sailfishconnect_sms
    JSON sailfishconnect_sms.json
    SOURCES smsplugin.cpp)

target_link_libraries(sailfishconnect_sms
PRIVATE
    kdeconnectcore
    KF5::I18n
    Qt5::DBus
    Qt5::Sql
)
