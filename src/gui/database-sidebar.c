#include "database-sidebar.h"

struct _DatabaseEntry
{
    GObject parent_instance;
    gchar *name;
    gchar *path;
    gboolean is_primary;
};

enum
{
    DB_PROP_0,
    DB_PROP_NAME,
    DB_PROP_PATH,
    DB_PROP_PRIMARY,
    DB_N_PROPS
};

static GParamSpec *db_properties[DB_N_PROPS];

G_DEFINE_FINAL_TYPE (DatabaseEntry, database_entry, G_TYPE_OBJECT)

static void
database_entry_finalize (GObject *object)
{
    DatabaseEntry *self = DATABASE_ENTRY (object);
    g_clear_pointer (&self->name, g_free);
    g_clear_pointer (&self->path, g_free);
    G_OBJECT_CLASS (database_entry_parent_class)->finalize (object);
}

static void
database_entry_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    DatabaseEntry *self = DATABASE_ENTRY (object);

    switch (prop_id)
    {
        case DB_PROP_NAME:
            g_value_set_string (value, self->name);
            break;
        case DB_PROP_PATH:
            g_value_set_string (value, self->path);
            break;
        case DB_PROP_PRIMARY:
            g_value_set_boolean (value, self->is_primary);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
database_entry_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    DatabaseEntry *self = DATABASE_ENTRY (object);

    switch (prop_id)
    {
        case DB_PROP_NAME:
            g_free (self->name);
            self->name = g_value_dup_string (value);
            break;
        case DB_PROP_PATH:
            g_free (self->path);
            self->path = g_value_dup_string (value);
            break;
        case DB_PROP_PRIMARY:
            self->is_primary = g_value_get_boolean (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
database_entry_class_init (DatabaseEntryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = database_entry_finalize;
    object_class->get_property = database_entry_get_property;
    object_class->set_property = database_entry_set_property;

    db_properties[DB_PROP_NAME] =
        g_param_spec_string ("name", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    db_properties[DB_PROP_PATH] =
        g_param_spec_string ("path", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    db_properties[DB_PROP_PRIMARY] =
        g_param_spec_boolean ("primary", NULL, NULL, FALSE,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties (object_class, DB_N_PROPS, db_properties);
}

static void
database_entry_init (DatabaseEntry *self)
{
    (void) self;
}

DatabaseEntry *
database_entry_new (const gchar *name,
                    const gchar *path)
{
    return g_object_new (DATABASE_TYPE_ENTRY,
                         "name", name,
                         "path", path,
                         NULL);
}

const gchar *
database_entry_get_name (DatabaseEntry *self)
{
    g_return_val_if_fail (DATABASE_IS_ENTRY (self), NULL);
    return self->name;
}

const gchar *
database_entry_get_path (DatabaseEntry *self)
{
    g_return_val_if_fail (DATABASE_IS_ENTRY (self), NULL);
    return self->path;
}

void
database_entry_set_name (DatabaseEntry *self,
                         const gchar   *name)
{
    g_return_if_fail (DATABASE_IS_ENTRY (self));

    if (g_strcmp0 (self->name, name) == 0)
        return;

    g_free (self->name);
    self->name = g_strdup (name);
    g_object_notify_by_pspec (G_OBJECT (self), db_properties[DB_PROP_NAME]);
}

gboolean
database_entry_get_primary (DatabaseEntry *self)
{
    g_return_val_if_fail (DATABASE_IS_ENTRY (self), FALSE);
    return self->is_primary;
}

void
database_entry_set_primary (DatabaseEntry *self,
                            gboolean       is_primary)
{
    g_return_if_fail (DATABASE_IS_ENTRY (self));

    if (self->is_primary == is_primary)
        return;

    self->is_primary = is_primary;
    g_object_notify_by_pspec (G_OBJECT (self), db_properties[DB_PROP_PRIMARY]);
}
