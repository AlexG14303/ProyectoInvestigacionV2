#include "followup-validator.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static void set_error(char *error, size_t error_size, const char *message) {
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

int cf_validate_compliance_status(const char *status, char *error, size_t error_size) {
    if (status == NULL || status[0] == '\0') {
        return 1;
    }

    if (strcmp(status, "SI_CUMPLE") == 0 ||
        strcmp(status, "PARCIAL") == 0 ||
        strcmp(status, "NO_CUMPLE") == 0) {
        return 1;
    }

    set_error(error, error_size, "compliance_status debe ser SI_CUMPLE, PARCIAL o NO_CUMPLE");
    return 0;
}

int cf_validate_required_create(int has_family_id, int has_record_number, char *error, size_t error_size) {
    if (!has_family_id) {
        set_error(error, error_size, "family_id es requerido");
        return 0;
    }
    if (!has_record_number) {
        set_error(error, error_size, "record_number es requerido");
        return 0;
    }
    return 1;
}

int cf_validate_iso_date(const char *date, char *error, size_t error_size) {
    if (date == NULL || date[0] == '\0') {
        return 1;
    }
    if (strlen(date) != 10 || date[4] != '-' || date[7] != '-') {
        set_error(error, error_size, "La fecha debe usar formato YYYY-MM-DD");
        return 0;
    }
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)date[i])) {
            set_error(error, error_size, "La fecha debe usar formato YYYY-MM-DD");
            return 0;
        }
    }

    int year  = (date[0]-'0')*1000 + (date[1]-'0')*100 + (date[2]-'0')*10 + (date[3]-'0');
    int month = (date[5]-'0')*10 + (date[6]-'0');
    int day   = (date[8]-'0')*10 + (date[9]-'0');

    if (month < 1 || month > 12) {
        set_error(error, error_size, "El mes debe estar entre 01 y 12");
        return 0;
    }

    static const int dias_por_mes[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_dia = dias_por_mes[month - 1];
    int es_bisiesto = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && es_bisiesto) {
        max_dia = 29;
    }

    if (day < 1 || day > max_dia) {
        set_error(error, error_size, "El dia no es valido para el mes indicado");
        return 0;
    }

    return 1;
}
