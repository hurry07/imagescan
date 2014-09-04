//  monitor.cpp -- available scanner devices
//  Copyright (C) 2012, 2013  SEIKO EPSON CORPORATION
//
//  License: GPL-3.0+
//  Author : AVASYS CORPORATION
//
//  This file is part of the 'Utsushi' package.
//  This package is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License or, at
//  your option, any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//  You ought to have received a copy of the GNU General Public License
//  along with this package.  If not, see <http://www.gnu.org/licenses/>.

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if HAVE_LIBUDEV
extern "C" {                    // needed until libudev-150
#include <libudev.h>
}
#include <list>
#endif
#if HAVE_LIBHAL
#include <libhal.h>
#endif

#include <cstdlib>

#include <boost/assert.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/regex.hpp>

#include "utsushi/log.hpp"
#include "utsushi/monitor.hpp"
#include "utsushi/run-time.hpp"

namespace utsushi {

using boost::filesystem::exists;
using boost::regex;
using boost::regex_match;
using boost::smatch;

class monitor::impl
{
public:
  impl ();

  static impl *instance_;

  monitor::container_type devices_;
};

monitor::impl *monitor::impl::instance_(0);

monitor::monitor ()
{
  if (!impl::instance_) {
    impl::instance_ = new monitor::impl ();
  }
}

monitor::const_iterator
monitor::begin () const
{
  return impl::instance_->devices_.begin ();
}

monitor::const_iterator
monitor::end () const
{
  return impl::instance_->devices_.end ();
}

bool
monitor::empty () const
{
  return impl::instance_->devices_.empty ();
}

monitor::size_type
monitor::size () const
{
  return impl::instance_->devices_.size ();
}

monitor::size_type
monitor::max_size () const
{
  return impl::instance_->devices_.max_size ();
}

monitor::const_iterator
monitor::find (const scanner::info& info) const
{
  return impl::instance_->devices_.find (info);
}

monitor::size_type
monitor::count (const scanner::info& info) const
{
  return impl::instance_->devices_.count (info);
}

monitor::container_type
monitor::read (std::istream& istr)
{
  container_type rv;

  const std::string key ("[[:alpha:]][[:alnum:]]*"
                         "(\\.[[:alpha:]][[:alnum:]]*)*");

  const regex section ("[[:blank:]]*"
                       "[[](" + key + ")[]]"
                       "[[:blank:]]*");

  const regex key_val ("[[:blank:]]*"
                       "(" + key + ")"
                       "[[:blank:]]*"
                       "="
                       "[[:blank:]]*"
                       "(.+)\\b"
                       "[[:blank:]]*");

  std::string common_prefix;
  std::map< std::string, std::string > kv;

  std::string line;
  size_t line_no (0);

  while (getline (istr, line))
    {
      ++line_no;

      if (line.empty ()
          || '#' == line[0]
          || ';' == line[0]
          || regex_match (line, regex ("[[:blank:]]*")))
        continue;

      smatch m;
      if (regex_match (line, m, section))
        {
          common_prefix = m[1];
          continue;
        }
      if (regex_match (line, m, key_val))
        {
          std::string key (common_prefix);
          if (!key.empty ()) key += ".";
          key += m[1];

          // Keep "uninteresting" keys out of kv
          if (0 != key.find ("devices.")) continue;

          if (!kv.insert (make_pair (key, m[3])).second)
            {
              log::error ("duplicate key:%1%:%2%") % line_no % line;
            }

          continue;
        }

      log::error ("parse error:%1%:%2%") % line_no % line;
    }

  // first collect the udi attribute prefixes

  const regex attr_key ("(" + key + ")\\.([[:alpha:]][[:alnum:]]*)");

  std::map< std::string, std::string >::const_iterator it;
  std::set< std::string > dev;

  for (it = kv.begin (); kv.end () != it; ++it)
    {
      smatch m;
      if (regex_match (it->first, m, attr_key))
        {
          if (m[3] == "udi") dev.insert (m[1]);
        }
      else
        {
          log::error ("internal error:%1%:%2%") % it->first % it->second;
        }
    }

  // for each udi attribute prefix insert a new scanner::info instance
  // with supported configured attributes set

  std::set< std::string >::const_iterator jt;
  for (jt = dev.begin (); dev.end () != jt; ++jt)
    {
      scanner::info info (kv[ *jt + ".udi"]);

      it = kv.find (*jt + ".name");
      if (kv.end () != it) info.name (it->second);

      it = kv.find (*jt + ".model");
      if (kv.end () != it) info.model (it->second);

      it = kv.find (*jt + ".vendor");
      if (kv.end () != it) info.vendor (it->second);

      rv.insert (info);
    }

  return rv;
}

static void
add_sane_udev (std::set<scanner::info>& devices, const char *key,
               const char *val);
static void
add_sane_hal  (std::set<scanner::info>& devices, const char *cap);

static void
add_conf_file  (std::set<scanner::info>& devices);

monitor::impl::impl ()
{
  add_conf_file (devices_);

  //  Pick up on any scanner devices that are tagged courtesy of the
  //  SANE project.  These functions assume that tags are set on the
  //  USB device rather than on the USB interface that corresponds to
  //  the scanner function of the device.  The implementations use a
  //  number of heuristics to divine the scanner's interface and use
  //  that when creating a scanner::info object.

  add_sane_udev (devices_, "libsane_matched", "yes");
  add_sane_hal  (devices_, "scanner");
}

#if (!HAVE_LIBUDEV)

static void
add_sane_udev (std::set<scanner::info>& devices, const char *key,
               const char *val)
{}

#else  /* HAVE_LIBUDEV */

//!  Gets a device's first degree offspring devices.
/*!  Very dumb convenience function to list the children of a udev
     device.  The library does not have any API to do so.

     \note  Caller needs to unref all listed devices in the return
            value.
     \todo  Beef up error checking/handling.
 */
static std::list<struct udev_device *>
udev_device_get_children (struct udev *ctx, struct udev_device *dev)
{
  std::list<struct udev_device *> result;

  struct udev_enumerate *it = udev_enumerate_new (ctx);
  if (!it)
    {
      return result;
    }

  udev_enumerate_scan_devices (it);

  struct udev_list_entry *ent;
  udev_list_entry_foreach (ent, udev_enumerate_get_list_entry (it))
    {
      const char *name = udev_list_entry_get_name (ent);
      struct udev_device *d = udev_device_new_from_syspath (ctx, name);

      if (d)
        {
          struct udev_device *p = udev_device_get_parent (d);

          if (p
              && (std::string (udev_device_get_syspath (dev))
                  == udev_device_get_syspath (p)))
            {
              result.push_back (udev_device_ref (d));
            }
        }
      udev_device_unref (d);
    }
  udev_enumerate_unref (it);

  return result;
}

//!  Picks up on SANE tagged scanner devices.
/*!  This function filters on a dedicated property, normally specified
     in the SANE project's udev rules file.

     \todo  Fine tune interface detection heuristics.
     \todo  Add support for non-USB subsystem devices.
     \todo  Beef up error checking/handling.
 */
static void
add_sane_udev (std::set<scanner::info>& devices, const char *key,
               const char *val)
{
  using std::string;

  struct udev *ctx = udev_new ();
  if (!ctx)
    {
      log::error ("udev_new");
      return;
    }

  struct udev_enumerate *loc = udev_enumerate_new (ctx);
  if (!loc)
    {
      log::error ("udev_enumerate_new");
      udev_unref (ctx);
      return;
    }

  udev_enumerate_add_match_property (loc, key, val);
  udev_enumerate_scan_devices (loc);

  struct udev_list_entry *ent;
  udev_list_entry_foreach (ent, udev_enumerate_get_list_entry (loc))
    {
      const char *name = udev_list_entry_get_name (ent);
      struct udev_device *dev = udev_device_new_from_syspath (ctx, name);

      if (dev)
        {
          const char *sub = udev_device_get_subsystem (dev);

          if (sub
              && 0 == string (sub).find ("usb"))
            {
              std::list<struct udev_device *> kids
                = udev_device_get_children (ctx, dev);

              std::list<struct udev_device *>::iterator it;
              for (it = kids.begin (); kids.end () != it; ++it)
                {
                  //  This only works because there is no dedicated
                  //  Linux kernel driver for scanner devices.  Of
                  //  course, this will also pick up any other device
                  //  without such a driver.

                  if (!udev_device_get_property_value (*it, "DRIVER"))
                    {
                      const char *mdl =
                        udev_device_get_property_value (dev, "ID_MODEL");
                      const char *vnd =
                        udev_device_get_property_value (dev, "ID_VENDOR");
                      const char *drv =
                        udev_device_get_property_value (dev, "utsushi_driver");

                      std::string cnx ("usb");
                      scanner::info info (cnx + "::"
                                          + udev_device_get_syspath (*it));
                      if (mdl) info.model (mdl);
                      if (vnd) info.vendor (vnd);
                      if (drv) info.driver (drv);

                      devices.insert (info);
                    }
                  udev_device_unref (*it);
                }
            }
          else
            {
              log::error ("unsupported subsystem: %1%") % sub;
            }
          udev_device_unref (dev);
        }
    }

  udev_enumerate_unref (loc);
  udev_unref (ctx);
}

#endif  /* HAVE_LIBUDEV */


#if (!HAVE_LIBHAL)

static void
add_sane_hal (std::set<scanner::info>& devices, const char *cap)
{}

#else  /* HAVE_LIBHAL */

//!  Picks up on SANE tagged scanner devices.
/*!  This function filters on the device capability \a cap, as
     normally specified in the SANE project's FDI file for use
     with HAL.

     \note  HAL is deprecated upstream in favour of udev.

     \todo  Fix interface detection heuristics.
     \todo  Add support for non-USB subsystem devices.
     \todo  Beef up error checking/handling.
 */
static void
add_sane_hal (std::set<scanner::info>& devices, const char *cap)
{
  log::alert ("HAL based scanner detection has been disabled.\n"
              "Contact the developers if support is needed.");

  // This code is supposed to have worked at some point in the past
  // when libudev didn't exist yet.  This is believed to have been at
  // the time when libsane merging a 'scanner' capability resulted in
  // that capability being set on the *device*.  With Ubuntu 10.04 and
  // Debian wheezy this is no longer the case.  The capability is set
  // on all of the device's *interfaces* breaking the detection logic.
  // Doing so results in a very interesting picture of multi-function
  // peripherals!
  //
  // With HAL being deprecated, there is little point in getting this
  // fixed unless really needed by someone somewhere where libudev is
  // not an option.

  return;

  using std::string;

  LibHalContext *ctx = libhal_ctx_new ();
  if (!ctx)
    {
      log::error ("libhal_ctx_new");
      return;
    }

  DBusError err;
  dbus_error_init (&err);
  DBusConnection *cnx = dbus_bus_get (DBUS_BUS_SYSTEM, &err);

  if (libhal_ctx_set_dbus_connection (ctx, cnx)
      && libhal_ctx_init (ctx, &err))
    {
      char **udi = NULL;
      int    cnt = 0;

      udi = libhal_find_device_by_capability (ctx, cap, &cnt, &err);

      if (!dbus_error_is_set (&err))
        {
          for (int i = 0; i < cnt; ++i)
            {
              char *sub = libhal_device_get_property_string
                (ctx, udi[i], "linux.subsystem", &err);

              if (!sub)
                {
                  if (dbus_error_is_set (&err))
                    {
                      log::error ("%1%: %2%") % err.name % err.message;
                    }
                  continue;
                }

              if (string ("usb") == sub)
                {
                  char **iface_udi = NULL;
                  int    iface_cnt = 0;

                  iface_udi = libhal_manager_find_device_string_match
                    (ctx, "info.parent", udi[i], &iface_cnt, &err);

                  for (int j = 0; j < iface_cnt; ++j)
                    {
                      //  This is based on the fact that all EPSON
                      //  devices encountered so far always use the
                      //  vendor specific class and there not being
                      //  much of a viable alternative in the specs.

                      if (255 == libhal_device_get_property_int
                          (ctx, iface_udi[j], "usb.interface.class", &err))
                        {
                          char *mdl = libhal_device_get_property_string
                            (ctx, iface_udi[j], "info.product", &err);
                          char *vnd = libhal_device_get_property_string
                            (ctx, iface_udi[j], "info.vendor", &err);
                          char *path = libhal_device_get_property_string
                            (ctx, iface_udi[j], "linux.sysfs_path", &err);

                          std::string cnx ("usb");
                          scanner::info info (cnx + "::" + path);
                          if (mdl) info.model (mdl);
                          if (vnd) info.vendor (vnd);

                          devices.insert (info);
                        }
                    }
                  libhal_free_string_array (iface_udi);
                }
            }
        }
      libhal_free_string_array (udi);
      libhal_ctx_shutdown (ctx, NULL);
    }

  if (dbus_error_is_set (&err))
    {
      log::error ("%1%: %2%") % err.name % err.message;
    }

  LIBHAL_FREE_DBUS_ERROR (&err);
  libhal_ctx_free (ctx);
}

#endif  /* HAVE_LIBHAL */


//! Picks up scanner devices from configuration files
/*! This function reads configuration files and merges scanner::info
 *  objects to the list of \a devices.  The file format is inspired
 *  by the informal Windows INI file format.
 *
 *  \sa http://en.wikipedia.org/wiki/INI_file
 *
 *  \todo  Read other files that the system wide configuration
 */
static void
add_conf_file (monitor::container_type& devices)
{
  run_time rt;
  std::string name (rt.conf_file (run_time::sys, PKGCONFFILE));
  std::ifstream ifs (name.c_str ());

  if (!ifs.is_open ())
    {
      if (exists (name))
        {
          log::error ("cannot open file: %1%") % name;
        }
      else
        {
          log::alert ("no such file: %1%") % name;
        }
      return;
    }

  monitor::container_type rv = monitor::read (ifs);
  devices.insert (rv.begin (), rv.end ());
}

}       // namespace utsushi