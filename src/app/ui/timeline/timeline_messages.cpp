#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/timeline/timeline.h"

#include "app/app.h"
#include "app/app_menus.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/doc.h"

#include "app/loop_tag.h"
#include "app/ui/app_menuitem.h"
#include "app/ui/configure_timeline_popup.h"
#include "app/ui/input_chain.h"
#include "app/ui_context.h"
#include "app/util/clipboard.h"
#include "base/scoped_value.h"
#include "doc/doc.h"
#include "os/system.h"
#include "ui/ui.h"

using namespace doc;
using namespace ui;

namespace app {

bool Timeline::onProcessMessage(Message* msg)
{
  if (msg->type() < kFirstRegisteredMessage) {
    auto handler = m_message_handlers[msg->type()];
    bool done = (this->*handler)(msg);
    if (done) {
      return true;
    }
  }

  return Widget::onProcessMessage(msg);
}

bool Timeline::onFocusEnterMessage(Message* msg) {
    App::instance()->inputChain().prioritize(this, msg);
    return false;
}

bool Timeline::onTimerMessage(Message* msg) {
  if (static_cast<TimerMessage*>(msg)->timer() == &m_clipboard_timer) {
    Doc* clipboard_document;
    DocRange clipboard_range;
    clipboard::get_document_range_info(
      &clipboard_document,
      &clipboard_range);

    if (isVisible() &&
        m_document &&
        m_document == clipboard_document) {
      // Set offset to make selection-movement effect
      if (m_offset_count < 7)
        m_offset_count++;
      else
        m_offset_count = 0;

      bool redrawOnlyMarchingAnts = getUpdateRegion().isEmpty();
      invalidateRect(gfx::Rect(getRangeBounds(clipboard_range)).offset(origin()));
      if (redrawOnlyMarchingAnts)
        m_redrawMarchingAntsOnly = true;
    }
    else if (m_clipboard_timer.isRunning()) {
      m_clipboard_timer.stop();
    }
  }
  return false;
}

bool Timeline::onMouseDownMessage(Message* msg) {
  MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);

  if (!m_document)
    return false;

  if (mouseMsg->middle() ||
      os::instance()->isKeyPressed(kKeySpace)) {
    captureMouse();
    m_state = STATE_SCROLLING;
    m_oldPos = static_cast<MouseMessage*>(msg)->position();
    return true;
  }

  // As we can ctrl+click color bar + timeline, now we have to
  // re-prioritize timeline on each click.
  App::instance()->inputChain().prioritize(this, msg);

  // Update hot part (as the user might have left clicked with
  // Ctrl on OS X, which it's converted to a right-click and it's
  // interpreted as other action by the Timeline::hitTest())
  setHot(hitTest(msg, mouseMsg->position() - bounds().origin()));

  // Clicked-part = hot-part.
  m_clk = m_hot;

  // With Ctrl+click (Win/Linux) or Shift+click (OS X) we can
  // select non-adjacents layer/frame ranges
  bool clearRange =
  #if !defined(__APPLE__)
    !msg->ctrlPressed() &&
  #endif
    !msg->shiftPressed();

  captureMouse();

  switch (m_hot.part) {

    case PART_SEPARATOR:
      m_state = STATE_MOVING_SEPARATOR;
      break;

    case PART_HEADER_EYE: {
      ASSERT(m_sprite);
      if (!m_sprite)
        break;

      bool regenRows = false;
      bool newVisibleState = !allLayersVisible();
      for (Layer* topLayer : m_sprite->root()->layers()) {
        if (topLayer->isVisible() != newVisibleState) {
          topLayer->setVisible(newVisibleState);
          if (topLayer->isGroup())
            regenRows = true;
        }
      }

      if (regenRows) {
        regenerateRows();
        invalidate();
      }

      // Redraw all views.
      m_document->notifyGeneralUpdate();
      break;
    }

    case PART_HEADER_PADLOCK: {
      ASSERT(m_sprite);
      if (!m_sprite)
        break;

      bool regenRows = false;
      bool newEditableState = !allLayersUnlocked();
      for (Layer* topLayer : m_sprite->root()->layers()) {
        if (topLayer->isEditable() != newEditableState) {
          topLayer->setEditable(newEditableState);
          if (topLayer->isGroup()) {
            regenRows = true;
          }
        }
      }

      if (regenRows) {
        regenerateRows();
        invalidate();
      }
      break;
    }

    case PART_HEADER_CONTINUOUS: {
      bool newContinuousState = !allLayersContinuous();
      for (size_t i=0; i<m_rows.size(); i++)
        m_rows[i].layer()->setContinuous(newContinuousState);
      invalidate();
      break;
    }

    case PART_HEADER_ONIONSKIN: {
      docPref().onionskin.active(!docPref().onionskin.active());
      invalidate();
      break;
    }
    case PART_HEADER_ONIONSKIN_RANGE_LEFT: {
      m_state = STATE_MOVING_ONIONSKIN_RANGE_LEFT;
      m_origFrames = docPref().onionskin.prevFrames();
      break;
    }
    case PART_HEADER_ONIONSKIN_RANGE_RIGHT: {
      m_state = STATE_MOVING_ONIONSKIN_RANGE_RIGHT;
      m_origFrames = docPref().onionskin.nextFrames();
      break;
    }
    case PART_HEADER_FRAME: {
      bool selectFrame = (mouseMsg->left() || !isFrameActive(m_clk.frame));

      if (selectFrame) {
        m_state = STATE_SELECTING_FRAMES;
        if (clearRange)
          clearAndInvalidateRange();
        m_range.startRange(m_layer, m_clk.frame, Range::kFrames);
        m_startRange = m_range;
        invalidateRange();

        setFrame(m_clk.frame, true);
      }
      break;
    }
    case PART_ROW_TEXT: {
      base::ScopedValue<bool> lock(m_fromTimeline, true, false);
      const layer_t old_layer = getLayerIndex(m_layer);
      const bool selectLayer = (mouseMsg->left() || !isLayerActive(m_clk.layer));
      const bool selectLayerInCanvas =
        (m_clk.layer != -1 &&
          mouseMsg->left() &&
          is_select_layer_in_canvas_key_pressed(mouseMsg));

      if (selectLayerInCanvas) {
        select_layer_boundaries(m_rows[m_clk.layer].layer(), m_frame,
                                get_select_layer_in_canvas_op(mouseMsg));
      }
      else if (selectLayer) {
        m_state = STATE_SELECTING_LAYERS;
        if (clearRange)
          clearAndInvalidateRange();
        m_range.startRange(m_rows[m_clk.layer].layer(),
                            m_frame, Range::kLayers);
        m_startRange = m_range;
        invalidateRange();

        // Did the user select another layer?
        if (old_layer != m_clk.layer) {
          setLayer(m_rows[m_clk.layer].layer());
          invalidate();
        }
      }

      // Change the scroll to show the new selected layer/cel.
      showCel(m_clk.layer, m_frame);
      break;
    }

    case PART_ROW_EYE_ICON:
      if (validLayer(m_clk.layer)) {
        Row& row = m_rows[m_clk.layer];
        Layer* layer = row.layer();
        ASSERT(layer)

        // Hide everything or restore alternative state
        bool oneWithInternalState = false;
        if (msg->altPressed()) {
          for (const Row& row : m_rows) {
            const Layer* l = row.layer();
            if (l->hasFlags(LayerFlags::Internal_WasVisible)) {
              oneWithInternalState = true;
              break;
            }
          }

          // If there is one layer with the internal state, restore the previous visible state
          if (oneWithInternalState) {
            for (Row& row : m_rows) {
              Layer* l = row.layer();
              if (l->hasFlags(LayerFlags::Internal_WasVisible)) {
                l->setVisible(true);
                l->switchFlags(LayerFlags::Internal_WasVisible, false);
              }
              else {
                l->setVisible(false);
              }
            }
          }
          // In other case, hide everything
          else {
            for (Row& row : m_rows) {
              Layer* l = row.layer();
              l->switchFlags(LayerFlags::Internal_WasVisible, l->isVisible());
              l->setVisible(false);
            }
          }

          regenerateRows();
          invalidate();

          m_document->notifyGeneralUpdate();
        }

        if (layer->isVisible() && !oneWithInternalState)
          m_state = STATE_HIDING_LAYERS;
        else
          m_state = STATE_SHOWING_LAYERS;

        setLayerVisibleFlag(m_clk.layer, m_state == STATE_SHOWING_LAYERS);
      }
      break;

    case PART_ROW_PADLOCK_ICON:
      if (validLayer(m_hot.layer)) {
        Row& row = m_rows[m_clk.layer];
        Layer* layer = row.layer();
        ASSERT(layer);
        if (layer->isEditable())
          m_state = STATE_LOCKING_LAYERS;
        else
          m_state = STATE_UNLOCKING_LAYERS;

        setLayerEditableFlag(m_clk.layer, m_state == STATE_UNLOCKING_LAYERS);
      }
      break;

    case PART_ROW_CONTINUOUS_ICON:
      if (validLayer(m_hot.layer)) {
        Row& row = m_rows[m_clk.layer];
        Layer* layer = row.layer();
        ASSERT(layer);

        if (layer->isImage()) {
          if (layer->isContinuous())
            m_state = STATE_DISABLING_CONTINUOUS_LAYERS;
          else
            m_state = STATE_ENABLING_CONTINUOUS_LAYERS;

          setLayerContinuousFlag(m_clk.layer, m_state == STATE_ENABLING_CONTINUOUS_LAYERS);
        }
        else if (layer->isGroup()) {
          if (layer->isCollapsed())
            m_state = STATE_EXPANDING_LAYERS;
          else
            m_state = STATE_COLLAPSING_LAYERS;

          setLayerCollapsedFlag(m_clk.layer, m_state == STATE_COLLAPSING_LAYERS);
          updateByMousePos(msg, ui::get_mouse_position() - bounds().origin());

          // The m_clk might have changed because we've
          // expanded/collapsed a group just right now (i.e. we've
          // called regenerateRows())
          m_clk = m_hot;

          ASSERT(m_rows[m_clk.layer].layer() == layer);
        }
      }
      break;

    case PART_CEL: {
      base::ScopedValue<bool> lock(m_fromTimeline, true, false);
      const layer_t old_layer = getLayerIndex(m_layer);
      const bool selectCel = (mouseMsg->left()
        || !isLayerActive(m_clk.layer)
        || !isFrameActive(m_clk.frame));
      const bool selectCelInCanvas =
        (m_clk.layer != -1 &&
          mouseMsg->left() &&
          is_select_layer_in_canvas_key_pressed(mouseMsg));
      const frame_t old_frame = m_frame;

      if (selectCelInCanvas) {
        select_layer_boundaries(m_rows[m_clk.layer].layer(),
                                m_clk.frame,
                                get_select_layer_in_canvas_op(mouseMsg));
      }
      else {
        if (selectCel) {
          m_state = STATE_SELECTING_CELS;
          if (clearRange)
            clearAndInvalidateRange();
          m_range.startRange(m_rows[m_clk.layer].layer(),
                              m_clk.frame, Range::kCels);
          m_startRange = m_range;
          invalidateRange();
        }

        // Select the new clicked-part.
        if (old_layer != m_clk.layer
            || old_frame != m_clk.frame) {
          setLayer(m_rows[m_clk.layer].layer());
          setFrame(m_clk.frame, true);
          invalidate();
        }
      }

      // Change the scroll to show the new selected cel.
      showCel(m_clk.layer, m_frame);
      invalidate();
      break;
    }
    case PART_RANGE_OUTLINE:
      m_state = STATE_MOVING_RANGE;

      // If we select the outline of a cels range, we have to
      // recalculate the dragged cel (m_clk) using a special
      // hitTestCel() and limiting the clicked cel inside the
      // range bounds.
      if (m_range.type() == Range::kCels) {
        m_clk = hitTestCel(mouseMsg->position() - bounds().origin());

        if (m_range.layers() > 0) {
          layer_t layerFirst, layerLast;
          if (selectedLayersBounds(selectedLayers(),
                                    &layerFirst, &layerLast)) {
            layer_t layerIdx = m_clk.layer;
            layerIdx = MID(layerFirst, layerIdx, layerLast);
            m_clk.layer = layerIdx;
          }
        }

        if (m_clk.frame < m_range.firstFrame())
          m_clk.frame = m_range.firstFrame();
        else if (m_clk.frame > m_range.lastFrame())
          m_clk.frame = m_range.lastFrame();
      }
      break;
  }

  // Redraw the new clicked part (header, layer or cel).
  invalidateHit(m_clk);
  return false;
}

bool Timeline::onMouseLeaveMessage(Message* msg) {
  if (m_hot.part != PART_NOTHING) {
    invalidateHit(m_hot);
    m_hot = Hit();
  }
  return false;
}

bool Timeline::onMouseMoveMessage(Message* msg) {
  if (!m_document)
    return false;

  gfx::Point mousePos = static_cast<MouseMessage*>(msg)->position()
    - bounds().origin();

  Hit hit;
  setHot(hit = hitTest(msg, mousePos));

  if (hasCapture()) {
    switch (m_state) {

      case STATE_SCROLLING: {
        gfx::Point absMousePos = static_cast<MouseMessage*>(msg)->position();
        setViewScroll(
          viewScroll() - gfx::Point(
            (absMousePos.x - m_oldPos.x),
            (absMousePos.y - m_oldPos.y)));

        m_oldPos = absMousePos;
        return true;
      }

      case STATE_MOVING_ONIONSKIN_RANGE_LEFT: {
        gfx::Rect onionRc = getOnionskinFramesBounds();

        int newValue = m_origFrames + (m_clk.frame - hit.frame);
        docPref().onionskin.prevFrames(MAX(0, newValue));

        onionRc |= getOnionskinFramesBounds();
        invalidateRect(onionRc.offset(origin()));
        return true;
      }

      case STATE_MOVING_ONIONSKIN_RANGE_RIGHT: {
        gfx::Rect onionRc = getOnionskinFramesBounds();

        int newValue = m_origFrames - (m_clk.frame - hit.frame);
        docPref().onionskin.nextFrames(MAX(0, newValue));

        onionRc |= getOnionskinFramesBounds();
        invalidateRect(onionRc.offset(origin()));
        return true;
      }

      case STATE_SHOWING_LAYERS:
      case STATE_HIDING_LAYERS:
        m_clk = hit;
        if (hit.part == PART_ROW_EYE_ICON) {
          setLayerVisibleFlag(hit.layer, m_state == STATE_SHOWING_LAYERS);
        }
        break;

      case STATE_LOCKING_LAYERS:
      case STATE_UNLOCKING_LAYERS:
        m_clk = hit;
        if (hit.part == PART_ROW_PADLOCK_ICON) {
          setLayerEditableFlag(hit.layer, m_state == STATE_UNLOCKING_LAYERS);
        }
        break;

      case STATE_ENABLING_CONTINUOUS_LAYERS:
      case STATE_DISABLING_CONTINUOUS_LAYERS:
        m_clk = hit;
        if (hit.part == PART_ROW_CONTINUOUS_ICON) {
          setLayerContinuousFlag(hit.layer, m_state == STATE_ENABLING_CONTINUOUS_LAYERS);
        }
        break;

      case STATE_EXPANDING_LAYERS:
      case STATE_COLLAPSING_LAYERS:
        m_clk = hit;
        if (hit.part == PART_ROW_CONTINUOUS_ICON) {
          setLayerCollapsedFlag(hit.layer, m_state == STATE_COLLAPSING_LAYERS);
          updateByMousePos(msg, ui::get_mouse_position() - bounds().origin());
        }
        break;

    }

    // If the mouse pressed the mouse's button in the separator,
    // we shouldn't change the hot (so the separator can be
    // tracked to the mouse's released).
    if (m_clk.part == PART_SEPARATOR) {
      m_separator_x = MAX(0, mousePos.x);
      layout();
      return true;
    }
  }

  updateDropRange(mousePos);

  if (hasCapture()) {
    switch (m_state) {

        case STATE_MOVING_RANGE: {
            frame_t newFrame;
            if (m_range.type() == Range::kLayers) {
              // If we are moving only layers we don't change the
              // current frame.
              newFrame = m_frame;
            }
            else {
              frame_t firstDrawableFrame;
              frame_t lastDrawableFrame;
              getDrawableFrames(&firstDrawableFrame, &lastDrawableFrame);

              if (hit.frame < firstDrawableFrame)
                newFrame = firstDrawableFrame - 1;
              else if (hit.frame > lastDrawableFrame)
                newFrame = lastDrawableFrame + 1;
              else
                newFrame = hit.frame;
            }

            layer_t newLayer;
            if (m_range.type() == Range::kFrames) {
              // If we are moving only frames we don't change the
              // current layer.
              newLayer = getLayerIndex(m_layer);
            }
            else {
              layer_t firstDrawableLayer;
              layer_t lastDrawableLayer;
              getDrawableLayers(&firstDrawableLayer, &lastDrawableLayer);

              if (hit.layer < firstDrawableLayer)
                newLayer = firstDrawableLayer - 1;
              else if (hit.layer > lastDrawableLayer)
                newLayer = lastDrawableLayer + 1;
              else
                newLayer = hit.layer;
            }

            showCel(newLayer, newFrame);
            break;
        }

      case STATE_SELECTING_LAYERS: {
        Layer* hitLayer = m_rows[hit.layer].layer();
        if (m_layer != hitLayer) {
          m_clk.layer = hit.layer;

          // We have to change the range before we generate an
          // onActiveSiteChange() event so observers (like cel
          // properties dialog) know the new selected range.
          m_range = m_startRange;
          m_range.endRange(hitLayer, m_frame);

          setLayer(hitLayer);
        }
        break;
      }

      case STATE_SELECTING_FRAMES: {
        invalidateRange();

        m_range = m_startRange;
        m_range.endRange(m_layer, hit.frame);

        setFrame(m_clk.frame = hit.frame, true);

        invalidateRange();
        break;
      }

      case STATE_SELECTING_CELS:
        Layer* hitLayer = m_rows[hit.layer].layer();
        if ((m_layer != hitLayer) || (m_frame != hit.frame)) {
          m_clk.layer = hit.layer;

          m_range = m_startRange;
          m_range.endRange(hitLayer, hit.frame);

          setLayer(hitLayer);
          setFrame(m_clk.frame = hit.frame, true);
        }
        break;
    }
  }

  updateStatusBar(msg);
  updateCelOverlayBounds(hit);
  return true;
}

bool Timeline::onMouseUpMessage(Message* msg) {
  if (hasCapture()) {
    ASSERT(m_document != NULL);

    MouseMessage* mouseMsg = static_cast<MouseMessage*>(msg);

    if (m_state == STATE_SCROLLING) {
      m_state = STATE_STANDBY;
      releaseMouse();
      return true;
    }

    bool regenRows = false;
    bool relayout = false;
    setHot(hitTest(msg, mouseMsg->position() - bounds().origin()));

    switch (m_hot.part) {

      case PART_HEADER_GEAR: {
        gfx::Rect gearBounds =
          getPartBounds(Hit(PART_HEADER_GEAR)).offset(bounds().origin());

        if (!m_confPopup) {
          ConfigureTimelinePopup* popup =
            new ConfigureTimelinePopup();

          popup->remapWindow();
          m_confPopup = popup;
        }

        if (!m_confPopup->isVisible()) {
          gfx::Rect bounds = m_confPopup->bounds();
          ui::fit_bounds(BOTTOM, gearBounds, bounds);

          m_confPopup->moveWindow(bounds);
          m_confPopup->openWindow();
        }
        else
          m_confPopup->closeWindow(NULL);
        break;
      }

      case PART_HEADER_FRAME:
        // Show the frame pop-up menu.
        if (mouseMsg->right()) {
          if (m_clk.frame == m_hot.frame) {
            Menu* popupMenu = AppMenus::instance()->getFramePopupMenu();
            if (popupMenu) {
              popupMenu->showPopup(mouseMsg->position());

              m_state = STATE_STANDBY;
              invalidate();
            }
          }
        }
        break;

      case PART_ROW_TEXT:
        // Show the layer pop-up menu.
        if (mouseMsg->right()) {
          if (m_clk.layer == m_hot.layer) {
            Menu* popupMenu = AppMenus::instance()->getLayerPopupMenu();
            if (popupMenu) {
              popupMenu->showPopup(mouseMsg->position());

              m_state = STATE_STANDBY;
              invalidate();
            }
          }
        }
        break;

      case PART_CEL: {
        // Show the cel pop-up menu.
        if (mouseMsg->right()) {
          Menu* popupMenu =
            (m_state == STATE_MOVING_RANGE &&
              m_range.type() == Range::kCels &&
              (m_hot.layer != m_clk.layer ||
              m_hot.frame != m_clk.frame)) ?
              AppMenus::instance()->getCelMovementPopupMenu():
              AppMenus::instance()->getCelPopupMenu();
          if (popupMenu) {
            popupMenu->showPopup(mouseMsg->position());

            // Do not drop in this function, the drop is done from
            // the menu in case we've used the
            // CelMovementPopupMenu
            m_state = STATE_STANDBY;
            invalidate();
          }
        }
        break;
      }

      case PART_TAG: {
        Tag* tag = m_clk.getTag();
        if (tag) {
          Params params;
          params.set("id", base::convert_to<std::string>(tag->id()).c_str());

          // As the m_clk.tag can be deleted with
          // RemoveTag command, we've to clean all references
          // to it from Hit() structures.
          cleanClk();
          m_hot = m_clk;

          if (mouseMsg->right()) {
            Menu* popupMenu = AppMenus::instance()->getTagPopupMenu();
            if (popupMenu) {
              AppMenuItem::setContextParams(params);
              popupMenu->showPopup(mouseMsg->position());
              AppMenuItem::setContextParams(Params());

              m_state = STATE_STANDBY;
              invalidate();
            }
          }
          else if (mouseMsg->left()) {
            Command* command = Commands::instance()
              ->byId(CommandId::FrameTagProperties());
            UIContext::instance()->executeCommand(command, params);
          }
        }
        break;
      }

      case PART_TAG_SWITCH_BAND_BUTTON:
        if (m_clk.band >= 0) {
          focusTagBand(m_clk.band);
          regenRows = true;
          relayout = true;
        }
        break;

    }

    if (regenRows) {
      regenerateRows();
      invalidate();
    }
    if (relayout)
      layout();

    if (m_state == STATE_MOVING_RANGE &&
        m_dropRange.type() != Range::kNone) {
      dropRange(is_copy_key_pressed(mouseMsg) ?
        Timeline::kCopy:
        Timeline::kMove);
    }

    // Clean the clicked-part & redraw the hot-part.
    cleanClk();

    if (hasCapture())
      invalidate();
    else
      invalidateHit(m_hot);

    // Restore the cursor.
    m_state = STATE_STANDBY;
    setCursor(msg, hitTest(msg, mouseMsg->position() - bounds().origin()));

    releaseMouse();
    updateStatusBar(msg);
    return true;
  }

  return false;
}

bool Timeline::onDoubleClickMessage(Message* msg) {
  switch (m_hot.part) {

      case PART_ROW_TEXT: {
        Command* command = Commands::instance()
          ->byId(CommandId::LayerProperties());

        UIContext::instance()->executeCommand(command);
        return true;
      }

      case PART_HEADER_FRAME: {
        Command* command = Commands::instance()
          ->byId(CommandId::FrameProperties());
        Params params;
        params.set("frame", "current");

        UIContext::instance()->executeCommand(command, params);
        return true;
      }

      case PART_CEL: {
        Command* command = Commands::instance()
          ->byId(CommandId::CelProperties());

        UIContext::instance()->executeCommand(command);
        return true;
      }

      case PART_TAG_BAND:
        if (m_hot.band >= 0) {
          focusTagBand(m_hot.band);
          regenerateRows();
          invalidate();
          layout();
          return true;
        }
        break;

  }

  return false;
}

bool Timeline::onKeyDownMessage(Message* msg) {
  bool used = false;

  switch (static_cast<KeyMessage*>(msg)->scancode()) {

    case kKeyEsc:
      if (m_state == STATE_STANDBY) {
        clearAndInvalidateRange();
      }
      else {
        m_state = STATE_STANDBY;
      }

      // Don't use this key, so it's caught by CancelCommand.
      // TODO The deselection of the current range should be
      // handled in CancelCommand itself.
      //used = true;
      break;

    case kKeySpace: {
      // If we receive a key down event when the Space bar is
      // pressed (because the Timeline has the keyboard focus) but
      // we don't have the mouse inside, we don't consume this
      // event so the Space bar can be used by the Editor to
      // activate the hand/pan/scroll tool.
      if (!hasMouse())
        break;

      m_scroll = true;
      used = true;
      break;
    }
  }

  updateByMousePos(msg, ui::get_mouse_position() - bounds().origin());
  if (used)
    return true;

  return false;
}

bool Timeline::onKeyUpMessage(Message* msg) {
  bool used = false;

  switch (static_cast<KeyMessage*>(msg)->scancode()) {

    case kKeySpace: {
      m_scroll = false;
      used = true;
      break;
    }
  }

  updateByMousePos(msg, ui::get_mouse_position() - bounds().origin());
  if (used)
    return true;

  return false;
}

bool Timeline::onMouseWheelMessage(Message* msg) {
  if (m_document) {
    gfx::Point delta = static_cast<MouseMessage*>(msg)->wheelDelta();
    const bool precise = static_cast<MouseMessage*>(msg)->preciseWheel();

    // Zoom timeline
    if (msg->ctrlPressed() || // TODO configurable
        msg->cmdPressed()) {
      double dz = delta.x + delta.y;

      if (precise) {
        dz /= 1.5;
        if (dz < -1.0) dz = -1.0;
        else if (dz > 1.0) dz = 1.0;
      }

      setZoomAndUpdate(m_zoom - dz, true);
    }
    else {
      if (!precise) {
        delta.x *= frameBoxWidth();
        delta.y *= layerBoxHeight();

        if (delta.x == 0 && // On macOS shift already changes the wheel axis
            msg->shiftPressed()) {
          if (std::fabs(delta.y) > delta.x)
            std::swap(delta.x, delta.y);
        }

        if (msg->altPressed()) {
          delta.x *= 3;
          delta.y *= 3;
        }
      }
      setViewScroll(viewScroll() + delta);
    }
  }

  return false;
}

bool Timeline::onSetCursorMessage(Message* msg) {
  if (m_document) {
    setCursor(msg, m_hot);
    return true;
  }

  return false;
}

bool Timeline::onTouchMagnifyMessage(Message* msg) {
  setZoomAndUpdate(
      m_zoom + m_zoom * static_cast<ui::TouchMessage*>(msg)->magnification(),
      true);

  return false;
}


}