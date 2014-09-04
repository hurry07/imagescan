//  preview.cpp -- display/control images before final acquisition
//  Copyright (C) 2012-2014  SEIKO EPSON CORPORATION
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

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include <boost/throw_exception.hpp>

#include <gtkmm/action.h>
#include <gtkmm/messagedialog.h>

#include <utsushi/i18n.hpp>
#include <utsushi/log.hpp>

#include "preview.hpp"
#include "../filters/jpeg.hpp"
#include "../filters/padding.hpp"
#include "../filters/pnm.hpp"
#include "../filters/threshold.hpp"

namespace utsushi {
namespace gtkmm {

using namespace _flt_;

using std::bad_alloc;
using std::logic_error;
using std::runtime_error;

preview::preview (BaseObjectType *ptr, Glib::RefPtr<Gtk::Builder>& builder)
  : base (ptr),
    zoom_(1.0), step_(0.1), zoom_min_(0.1), zoom_max_(2.5),
    interp_(Gdk::INTERP_BILINEAR),
    loader_(0), pixbuf_(0)
{
  odevice_ = odevice::ptr (this, null_deleter ());

  builder->get_widget ("preview-window", window_);
  if (!window_)
    BOOST_THROW_EXCEPTION
      (logic_error ("Dialog specification requires a 'preview-window'"));
  window_->add (event_box_);
  event_box_.add_events (Gdk::EXPOSURE_MASK);
  event_box_.signal_expose_event ()
    .connect (sigc::mem_fun (*this, &preview::on_expose_event));
  event_box_.add (image_);
  image_.set_alignment (Gtk::ALIGN_LEFT, Gtk::ALIGN_TOP);

  Glib::RefPtr<Glib::Object> obj = builder->get_object ("uimanager");
  ui_ = Glib::RefPtr<Gtk::UIManager>::cast_dynamic (obj);
  if (!ui_)
    BOOST_THROW_EXCEPTION
      (logic_error ("Dialog specification requires a 'uimanager'"));

  Glib::RefPtr<Gtk::Action> a;

  a = ui_->get_action ("/preview/refresh");
  if (a) {
    a->signal_activate ()
      .connect (sigc::mem_fun (*this, &preview::on_refresh));
  }
  a = ui_->get_action ("/preview/zoom-in");
  if (a) {
    a->signal_activate ()
      .connect (sigc::mem_fun (*this, &preview::on_zoom_in));
  }
  a = ui_->get_action ("/preview/zoom-out");
  if (a) {
    a->signal_activate ()
      .connect (sigc::mem_fun (*this, &preview::on_zoom_out));
  }
  a = ui_->get_action ("/preview/zoom-100");
  if (a) {
    a->signal_activate ()
      .connect (sigc::mem_fun (*this, &preview::on_zoom_100));
  }
  a = ui_->get_action ("/preview/zoom-fit");
  if (a) {
    a->signal_activate ()
      .connect (sigc::mem_fun (*this, &preview::on_zoom_fit));
  }

  set_sensitive ();
  show_all ();
}

streamsize
preview::write (const octet *data, streamsize n)
{
  if (0 < n)
    loader_->write (reinterpret_cast<const guint8 *> (data), n);
  return n;
}

void
preview::boi (const context& ctx)
{
  loader_ = Gdk::PixbufLoader::create ();
  if (!loader_)
    BOOST_THROW_EXCEPTION
      (bad_alloc ());

  loader_->signal_area_prepared ()
    .connect (sigc::mem_fun (*this, &preview::on_area_prepared));
  loader_->signal_area_updated ()
    .connect (sigc::mem_fun (*this, &preview::on_area_updated));

  ctx_  = ctx;
  zoom_ = get_zoom_factor (ctx_.width (), ctx_.height ());
}

void
preview::eoi (const context& ctx)
{
  loader_->close ();
  loader_.reset ();
}

void
preview::set_sensitive ()
{
  if (!ui_) return;

  Glib::RefPtr<Gtk::Action> a;

  a = ui_->get_action ("/preview/refresh");
  if (a) { a->set_sensitive (idevice_); }
  a = ui_->get_action ("/preview/zoom-in");
  if (a) { a->set_sensitive (pixbuf_ && (zoom_ < zoom_max_)); }
  a = ui_->get_action ("/preview/zoom-out");
  if (a) { a->set_sensitive (pixbuf_ && (zoom_ > zoom_min_)); }
  a = ui_->get_action ("/preview/zoom-100");
  if (a) { a->set_sensitive (pixbuf_); }
  a = ui_->get_action ("/preview/zoom-fit");
  if (a) { a->set_sensitive (pixbuf_); }
}

void
preview::scale ()
{
  if (!pixbuf_) return;

  if (zoom_ < zoom_min_) zoom_ = zoom_min_;
  if (zoom_ > zoom_max_) zoom_ = zoom_max_;

  int w = zoom_ * pixbuf_->get_width ();
  int h = zoom_ * pixbuf_->get_height ();

  scaled_pixbuf_ = pixbuf_->scale_simple (w, h, interp_);
  image_.set (scaled_pixbuf_);

  set_sensitive ();
}

double
preview::get_zoom_factor (double width, double height)
{
  int scrollbar_spacing;
  window_->get_style_property ("scrollbar-spacing", scrollbar_spacing);

  int margin = window_->get_border_width () + scrollbar_spacing + 2;
  double zoom_width  = window_->get_width ()  - 2 * margin;
  double zoom_height = window_->get_height () - 2 * margin;

  zoom_width  /= width;
  zoom_height /= height;

  return std::min (zoom_width, zoom_height);
}

void
preview::on_area_prepared ()
{
  pixbuf_ = loader_->get_pixbuf ();
  set_sensitive ();
}

void
preview::on_area_updated (int x, int y, int width, int height)
{
  if (!pixbuf_) return;

  // We go for scaling speed here to get decent progressive display.
  // Once the image is complete it will be redisplayed with a better
  // looking scaling algorithm.

  Glib::RefPtr< Gdk::Pixbuf > scaled (pixbuf_->scale_simple
                                      (zoom_ * ctx_.width (),
                                       zoom_ * ctx_.height (),
                                       Gdk::INTERP_NEAREST));
  image_.get_window ()->draw_pixbuf (scaled, 0, 0, 0, 0,
                                     scaled->get_width (),
                                     scaled->get_height (),
                                     Gdk::RGB_DITHER_NONE, 0, 0);
}

void
preview::on_refresh ()
{
  value resolution;
  try {
    option opt ((*control_)["resolution"]);

    resolution = opt;
    opt = opt.constraint ()->default_value ();
  }
  catch (const std::out_of_range&){}

  value image_count;
  try {
    image_count = (*control_)["image-count"];
    (*control_)["image-count"] = 1;
  }
  catch (const std::out_of_range&){}

  value duplex;
  try {
    duplex = (*control_)["duplex"];
    (*control_)["duplex"] = toggle (false);
  }
  catch (const std::out_of_range&){}

  try
    {
      const std::string xfer_raw = "image/x-raster";
      const std::string xfer_jpg = "image/jpeg";
      std::string xfer_fmt = idevice_->get_context ().content_type ();

      bool bilevel = ((*control_)["image-type"] == "Gray (1 bit)");

      toggle force_extent = true;
      quantity width  = -1.0;
      quantity height = -1.0;
      try
        {
          force_extent = value ((*control_)["force-extent"]);
          width   = value ((*control_)["br-x"]);
          width  -= value ((*control_)["tl-x"]);
          height  = value ((*control_)["br-y"]);
          height -= value ((*control_)["tl-y"]);
        }
      catch (const std::out_of_range&)
        {
          force_extent = false;
          width  = -1.0;
          height = -1.0;
        }
      if (force_extent) force_extent = (width > 0 || height > 0);

      //! \todo decide what to do WRT resampling

      filter::ptr threshold (make_shared< threshold > ());
      try
        {
          (*threshold->options ())["threshold"]
            = value ((*control_)["threshold"]);
        }
      catch (std::out_of_range&)
        {
          log::error ("Falling back to default threshold value");
        }

      filter::ptr jpeg_compress (make_shared< jpeg::compressor > ());
      try
        {
          (*jpeg_compress->options ())["quality"]
            = value ((*control_)["jpeg-quality"]);
        }
      catch (const std::out_of_range&)
        {
          log::error ("Falling back to default JPEG compression quality");
        }

      stream_ = make_shared< stream > ();
      /**/ if (xfer_raw == xfer_fmt)
        {
          stream_->push (make_shared< padding > ());
          if (force_extent)
            stream_->push (make_shared< bottom_padder > (width, height));
          stream_->push (make_shared< pnm > ());
        }
      else if (xfer_jpg == xfer_fmt)
        {
          stream_->push (make_shared< jpeg::decompressor > ());
          if (bilevel) stream_->push (threshold);
          if (force_extent)
            stream_->push (make_shared< bottom_padder > (width, height));
          stream_->push (make_shared< pnm > ());
        }
      else
        {
          /*! \todo We're blindly assuming Gdk::PixbufLoader can
           *        handle image_fmt.  We should check the supported
           *        formats to confirm and take action if it doesn't.
           */
          if (force_extent)
            log::alert ("extent forcing support not implemented");
        }
      stream_->push (odevice::ptr (odevice_));

      *idevice_ | *stream_;
      scale ();
    }
  catch (const runtime_error& e)
    {
      log::error (e.what ());

      Gtk::MessageDialog dialog (e.what (), false,
                                 Gtk::MESSAGE_WARNING);
      dialog.set_keep_above ();
      dialog.run ();

      if (loader_)
        loader_->close ();
      loader_.reset ();
      pixbuf_.reset ();
    }

  if (value () != duplex)
    {
      (*control_)["duplex"] = duplex;
    }
  if (value () != image_count)
    {
      (*control_)["image-count"] = image_count;
    }
  if (value () != resolution)
    {
      (*control_)["resolution"] = resolution;
    }
}

void
preview::on_zoom_in ()
{
  zoom_ += step_;
  scale ();
}

void
preview::on_zoom_out ()
{
  zoom_ -= step_;
  scale ();
}

void
preview::on_zoom_100 ()
{
  if (!pixbuf_) return;

  scaled_pixbuf_ = pixbuf_;
  image_.set (scaled_pixbuf_);

  zoom_ = 1.00;
  set_sensitive ();
}

void
preview::on_zoom_fit ()
{
  if (!pixbuf_) return;

  zoom_ = get_zoom_factor (pixbuf_->get_width (), pixbuf_->get_height ());
  scale ();
}

void
preview::on_device_changed (scanner::ptr s)
{
  // keep the base class APIs apart until convinced that the preview
  // really needs the scanner API
  idevice_ = s;
  control_ = s->options ();

  pixbuf_.reset ();
  image_.clear ();
  set_sensitive ();
}

bool
preview::on_expose_event (GdkEventExpose *event)
{
  return base::on_expose_event (event);
}

}       // namespace gtkmm
}       // namespace utsushi