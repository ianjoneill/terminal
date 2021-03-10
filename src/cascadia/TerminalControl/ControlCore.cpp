// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlCore.h"
#include <argb.h>
#include <DefaultSettings.h>
#include <unicode.hpp>
#include <Utf16Parser.hpp>
#include <Utils.h>
#include <WinUser.h>
#include <LibraryResources.h>
#include "../../types/inc/GlyphWidth.hpp"
#include "../../types/inc/Utils.hpp"
#include "../../buffer/out/search.h"

#include "ControlCore.g.cpp"
// #include "TermControlAutomationPeer.h" // ?

using namespace ::Microsoft::Console::Types;
using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;

namespace winrt::Microsoft::Terminal::TerminalControl::implementation
{
    // Helper static function to ensure that all ambiguous-width glyphs are reported as narrow.
    // See microsoft/terminal#2066 for more info.
    static bool _IsGlyphWideForceNarrowFallback(const std::wstring_view /* glyph */)
    {
        return false; // glyph is not wide.
    }

    static bool _EnsureStaticInitialization()
    {
        // use C++11 magic statics to make sure we only do this once.
        static bool initialized = []() {
            // *** THIS IS A SINGLETON ***
            SetGlyphWidthFallback(_IsGlyphWideForceNarrowFallback);

            return true;
        }();
        return initialized;
    }

    ControlCore::ControlCore(IControlSettings settings,
                             TerminalConnection::ITerminalConnection connection) :
        _connection{ connection },
        _settings{ settings },
        _desiredFont{ DEFAULT_FONT_FACE, 0, DEFAULT_FONT_WEIGHT, { 0, DEFAULT_FONT_SIZE }, CP_UTF8 },
        _actualFont{ DEFAULT_FONT_FACE, 0, DEFAULT_FONT_WEIGHT, { 0, DEFAULT_FONT_SIZE }, CP_UTF8, false }
    {
        _EnsureStaticInitialization();

        _terminal = std::make_unique<::Microsoft::Terminal::Core::Terminal>();

        // Subscribe to the connection's disconnected event and call our connection closed handlers.
        _connectionStateChangedRevoker = _connection.StateChanged(winrt::auto_revoke, [this](auto&& /*s*/, auto&& /*v*/) {
            _ConnectionStateChangedHandlers(*this, nullptr);
        });

        // This event is explicitly revoked in the destructor: does not need weak_ref
        auto onReceiveOutputFn = [this](const hstring str) {
            _terminal->Write(str);

            // !TODO!: _updatePatternLocations should happen in the Core, but it's
            // a ThrottledFunc, which needs a CoreDispatcher. How do we plan on
            // dealing with that?
            // _updatePatternLocations->Run();
        };
        _connectionOutputEventToken = _connection.TerminalOutput(onReceiveOutputFn);

        _terminal->SetWriteInputCallback([this](std::wstring& wstr) {
            _SendInputToConnection(wstr);
        });

        // GH#8969: pre-seed working directory to prevent potential races
        _terminal->SetWorkingDirectory(_settings.StartingDirectory());

        auto pfnCopyToClipboard = std::bind(&ControlCore::_TerminalCopyToClipboard, this, std::placeholders::_1);
        _terminal->SetCopyToClipboardCallback(pfnCopyToClipboard);

        auto pfnWarningBell = std::bind(&ControlCore::_TerminalWarningBell, this);
        _terminal->SetWarningBellCallback(pfnWarningBell);

        auto pfnTitleChanged = std::bind(&ControlCore::_TerminalTitleChanged, this, std::placeholders::_1);
        _terminal->SetTitleChangedCallback(pfnTitleChanged);

        auto pfnTabColorChanged = std::bind(&ControlCore::_TerminalTabColorChanged, this, std::placeholders::_1);
        _terminal->SetTabColorChangedCallback(pfnTabColorChanged);

        auto pfnBackgroundColorChanged = std::bind(&ControlCore::_TerminalBackgroundColorChanged, this, std::placeholders::_1);
        _terminal->SetBackgroundCallback(pfnBackgroundColorChanged);

        auto pfnScrollPositionChanged = std::bind(&ControlCore::_TerminalScrollPositionChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        _terminal->SetScrollPositionChangedCallback(pfnScrollPositionChanged);

        auto pfnTerminalCursorPositionChanged = std::bind(&ControlCore::_TerminalCursorPositionChanged, this);
        _terminal->SetCursorPositionChangedCallback(pfnTerminalCursorPositionChanged);

        auto pfnTerminalTaskbarProgressChanged = std::bind(&ControlCore::_TerminalTaskbarProgressChanged, this);
        _terminal->TaskbarProgressChangedCallback(pfnTerminalTaskbarProgressChanged);

        _terminal->UpdateSettings(settings);
    }

    bool ControlCore::InitializeTerminal(const double actualWidth,
                                         const double actualHeight,
                                         const double compositionScaleX,
                                         const double compositionScaleY)
    {
        _panelWidth = actualWidth;
        _panelHeight = actualHeight;
        _compositionScaleX = compositionScaleX;
        _compositionScaleY = compositionScaleY;

        { // scope for terminalLock
            auto terminalLock = _terminal->LockForWriting();

            if (_initializedTerminal)
            {
                return false;
            }

            const auto windowWidth = actualWidth * compositionScaleX; // Width() and Height() are NaN?
            const auto windowHeight = actualHeight * compositionScaleY;

            if (windowWidth == 0 || windowHeight == 0)
            {
                return false;
            }

            // First create the render thread.
            // Then stash a local pointer to the render thread so we can initialize it and enable it
            // to paint itself *after* we hand off its ownership to the renderer.
            // We split up construction and initialization of the render thread object this way
            // because the renderer and render thread have circular references to each other.
            auto renderThread = std::make_unique<::Microsoft::Console::Render::RenderThread>();
            auto* const localPointerToThread = renderThread.get();

            // Now create the renderer and initialize the render thread.
            _renderer = std::make_unique<::Microsoft::Console::Render::Renderer>(_terminal.get(), nullptr, 0, std::move(renderThread));
            ::Microsoft::Console::Render::IRenderTarget& renderTarget = *_renderer;

            // !TODO!: We _DO_ want this
            // _renderer->SetRendererEnteredErrorStateCallback([weakThis = get_weak()]() {
            //     if (auto strongThis{ weakThis.get() })
            //     {
            //         strongThis->_RendererEnteredErrorState();
            //     }
            // });

            THROW_IF_FAILED(localPointerToThread->Initialize(_renderer.get()));

            // Set up the DX Engine
            auto dxEngine = std::make_unique<::Microsoft::Console::Render::DxEngine>();
            _renderer->AddRenderEngine(dxEngine.get());

            // Initialize our font with the renderer
            // We don't have to care about DPI. We'll get a change message immediately if it's not 96
            // and react accordingly.
            _UpdateFont(true);

            const COORD windowSize{ static_cast<short>(windowWidth),
                                    static_cast<short>(windowHeight) };

            // Fist set up the dx engine with the window size in pixels.
            // Then, using the font, get the number of characters that can fit.
            // Resize our terminal connection to match that size, and initialize the terminal with that size.
            const auto viewInPixels = Viewport::FromDimensions({ 0, 0 }, windowSize);
            LOG_IF_FAILED(dxEngine->SetWindowSize({ viewInPixels.Width(), viewInPixels.Height() }));

            // Update DxEngine's SelectionBackground
            dxEngine->SetSelectionBackground(_settings.SelectionBackground());

            const auto vp = dxEngine->GetViewportInCharacters(viewInPixels);
            const auto width = vp.Width();
            const auto height = vp.Height();
            _connection.Resize(height, width);

            // Override the default width and height to match the size of the swapChainPanel
            _settings.InitialCols(width);
            _settings.InitialRows(height);

            _terminal->CreateFromSettings(_settings, renderTarget);

            // IMPORTANT! Set this callback up sooner than later. If we do it
            // after Enable, then it'll be possible to paint the frame once
            // _before_ the warning handler is set up, and then warnings from
            // the first paint will be ignored!
            dxEngine->SetWarningCallback(std::bind(&ControlCore::_RendererWarning, this, std::placeholders::_1));

            // Tell the DX Engine to notify us when the swap chain changes.
            // We do this after we initially set the swapchain so as to avoid unnecessary callbacks (and locking problems)
            _renderEngine->SetCallback(std::bind(&ControlCore::RenderEngineSwapChainChanged, this));

            dxEngine->SetRetroTerminalEffect(_settings.RetroTerminalEffect());
            dxEngine->SetPixelShaderPath(_settings.PixelShaderPath());
            dxEngine->SetForceFullRepaintRendering(_settings.ForceFullRepaintRendering());
            dxEngine->SetSoftwareRendering(_settings.SoftwareRendering());

            // Update DxEngine's AntialiasingMode
            switch (_settings.AntialiasingMode())
            {
            case TextAntialiasingMode::Cleartype:
                dxEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
                break;
            case TextAntialiasingMode::Aliased:
                dxEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
                break;
            case TextAntialiasingMode::Grayscale:
            default:
                dxEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                break;
            }

            // GH#5098: Inform the engine of the opacity of the default text background.
            if (_settings.UseAcrylic())
            {
                dxEngine->SetDefaultTextBackgroundOpacity(::base::saturated_cast<float>(_settings.TintOpacity()));
            }

            THROW_IF_FAILED(dxEngine->Enable());
            _renderEngine = std::move(dxEngine);

            // !TODO! in the past we did _AttachDxgiSwapChainToXaml _before_ calling
            // EnablePainting. Mild worry that doing EnablePainting first will
            // break
            localPointerToThread->EnablePainting();

            _initializedTerminal = true;
        } // scope for TerminalLock

        // Start the connection outside of lock, because it could
        // start writing output immediately.
        _connection.Start();

        // !TODO!: Do we want this?
        // Likewise, run the event handlers outside of lock (they could
        // be reentrant)
        // _InitializedHandlers(*this, nullptr);
        return true;
    }
    // Method Description:
    // - Writes the given sequence as input to the active terminal connection.
    // - This method has been overloaded to allow zero-copy winrt::param::hstring optimizations.
    // Arguments:
    // - wstr: the string of characters to write to the terminal connection.
    // Return Value:
    // - <none>
    void ControlCore::_SendInputToConnection(const winrt::hstring& wstr)
    {
        if (_isReadOnly)
        {
            // !TODO!: Do we want this?
            // _RaiseReadOnlyWarning();
        }
        else
        {
            _connection.WriteInput(wstr);
        }
    }

    void ControlCore::_SendInputToConnection(std::wstring_view wstr)
    {
        if (_isReadOnly)
        {
            // !TODO!: Do we want this?
            // _RaiseReadOnlyWarning();
        }
        else
        {
            _connection.WriteInput(wstr);
        }
    }

    // Method Description:
    // - Writes the given sequence as input to the active terminal connection,
    // Arguments:
    // - wstr: the string of characters to write to the terminal connection.
    // Return Value:
    // - <none>
    void ControlCore::SendInput(const winrt::hstring& wstr)
    {
        _SendInputToConnection(wstr);
    }

    bool ControlCore::SendCharEvent(const wchar_t ch,
                                    const WORD scanCode,
                                    const ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        return _terminal->SendCharEvent(ch, scanCode, modifiers);
    }

    // Method Description:
    // - Send this particular key event to the terminal.
    //   See Terminal::SendKeyEvent for more information.
    // - Clears the current selection.
    // - Makes the cursor briefly visible during typing.
    // Arguments:
    // - vkey: The vkey of the key pressed.
    // - scanCode: The scan code of the key pressed.
    // - states: The Microsoft::Terminal::Core::ControlKeyStates representing the modifier key states.
    // - keyDown: If true, the key was pressed, otherwise the key was released.
    bool ControlCore::TrySendKeyEvent(const WORD vkey,
                                      const WORD scanCode,
                                      const ControlKeyStates modifiers,
                                      const bool eitherWinPressed,
                                      const bool keyDown)
    {
        // When there is a selection active, escape should clear it and NOT flow through
        // to the terminal. With any other keypress, it should clear the selection AND
        // flow through to the terminal.
        // GH#6423 - don't dismiss selection if the key that was pressed was a
        // modifier key. We'll wait for a real keystroke to dismiss the
        // GH #7395 - don't dismiss selection when taking PrintScreen
        // selection.
        // GH#8522, GH#3758 - Only dismiss the selection on key _down_. If we
        // dismiss on key up, then there's chance that we'll immediately dismiss
        // a selection created by an action bound to a keydown.
        if (HasSelection() &&
            !KeyEvent::IsModifierKey(vkey) &&
            vkey != VK_SNAPSHOT &&
            keyDown)
        {
            // GH#8791 - don't dismiss selection if Windows key was also pressed as a key-combination.
            if (!eitherWinPressed)
            {
                _terminal->ClearSelection();
                _renderer->TriggerSelection();
            }

            if (vkey == VK_ESCAPE)
            {
                return true;
            }
        }

        // If the terminal translated the key, mark the event as handled.
        // This will prevent the system from trying to get the character out
        // of it and sending us a CharacterReceived event.
        const auto handled = vkey ? _terminal->SendKeyEvent(vkey,
                                                            scanCode,
                                                            modifiers,
                                                            keyDown) :
                                    true;

        return handled;
    }

    bool ControlCore::SendMouseEvent(const COORD viewportPos,
                                     const unsigned int uiButton,
                                     const ControlKeyStates states,
                                     const short wheelDelta,
                                     const ::Microsoft::Console::VirtualTerminal::TerminalInput::MouseButtonState state)
    {
        return _terminal->SendMouseEvent(viewportPos, uiButton, states, wheelDelta, state);
    }
    void ControlCore::UserScrollViewport(const int viewTop)
    {
        // Clear the regex pattern tree so the renderer does not try to render them while scrolling
        _terminal->ClearPatternTree();

        // This is a scroll event that wasn't initiated by the terminal
        //      itself - it was initiated by the mouse wheel, or the scrollbar.
        _terminal->UserScrollViewport(viewTop);
    }

    void ControlCore::ToggleShaderEffects()
    {
        auto lock = _terminal->LockForWriting();
        // Originally, this action could be used to enable the retro effects
        // even when they're set to `false` in the settings. If the user didn't
        // specify a custom pixel shader, manually enable the legacy retro
        // effect first. This will ensure that a toggle off->on will still work,
        // even if they currently have retro effect off.
        if (_settings.PixelShaderPath().empty() && !_renderEngine->GetRetroTerminalEffect())
        {
            // SetRetroTerminalEffect to true will enable the effect. In this
            // case, the shader effect will already be disabled (because neither
            // a pixel shader nor the retro effects were originally requested).
            // So we _don't_ want to toggle it again below, because that would
            // toggle it back off.
            _renderEngine->SetRetroTerminalEffect(true);
        }
        else
        {
            _renderEngine->ToggleShaderEffects();
        }
    }

    // Method description:
    // - Updates last hovered cell, renders / removes rendering of hyper-link if required
    // Arguments:
    // - terminalPosition: The terminal position of the pointer
    void ControlCore::_UpdateHoveredCell(const std::optional<COORD>& terminalPosition)
    {
        if (terminalPosition == _lastHoveredCell)
        {
            return;
        }

        _lastHoveredCell = terminalPosition;

        const uint16_t newId = terminalPosition.has_value() ? _terminal->GetHyperlinkIdAtPosition(*terminalPosition) :
                                                              0u;
        const auto newInterval = terminalPosition.has_value() ? _terminal->GetHyperlinkIntervalFromPosition(*terminalPosition) :
                                                                std::nullopt;

        // If the hyperlink ID changed or the interval changed, trigger a redraw all
        // (so this will happen both when we move onto a link and when we move off a link)
        if (newId != _lastHoveredId ||
            (newInterval != _lastHoveredInterval))
        {
            _lastHoveredId = newId;
            _lastHoveredInterval = newInterval;
            _renderEngine->UpdateHyperlinkHoveredId(newId);
            _renderer->UpdateLastHoveredInterval(newInterval);
            _renderer->TriggerRedrawAll();

            _raiseHoveredHyperlinkChanged();
        }
    }

    void ControlCore::_raiseHoveredHyperlinkChanged()
    {
        _HoveredHyperlinkChangedHandlers(*this, nullptr);
    }

    winrt::hstring ControlCore::GetHyperlink(const til::point pos) const
    {
        return winrt::hstring{ _terminal->GetHyperlinkAtPosition(pos) };
    }

    winrt::hstring ControlCore::GetHoveredUriText() const
    {
        if (_lastHoveredCell.has_value())
        {
            const winrt::hstring uri{ _terminal->GetHyperlinkAtPosition(*_lastHoveredCell) };
            return uri;
        }
        return { L"" };
    }

    std::optional<COORD> ControlCore::GetHoveredCell() const
    {
        return _lastHoveredCell;
    }

    void ControlCore::UpdateSettings(const IControlSettings& settings)
    {
        _settings = settings;

        auto lock = _terminal->LockForWriting();

        // Initialize our font information.
        const auto fontFace = _settings.FontFace();
        const short fontHeight = gsl::narrow_cast<short>(_settings.FontSize());
        const auto fontWeight = _settings.FontWeight();
        // The font width doesn't terribly matter, we'll only be using the
        //      height to look it up
        // The other params here also largely don't matter.
        //      The family is only used to determine if the font is truetype or
        //      not, but DX doesn't use that info at all.
        //      The Codepage is additionally not actually used by the DX engine at all.
        _actualFont = { fontFace, 0, fontWeight.Weight, { 0, fontHeight }, CP_UTF8, false };
        _desiredFont = { _actualFont };

        // Update the terminal core with its new Core settings
        _terminal->UpdateSettings(_settings);

        if (!_initializedTerminal)
        {
            // If we haven't initialized, there's no point in continuing.
            // Initialization will handle the renderer settings.
            return;
        }

        // Update DxEngine settings under the lock
        _renderEngine->SetSelectionBackground(_settings.SelectionBackground());

        _renderEngine->SetRetroTerminalEffect(_settings.RetroTerminalEffect());
        _renderEngine->SetPixelShaderPath(_settings.PixelShaderPath());
        _renderEngine->SetForceFullRepaintRendering(_settings.ForceFullRepaintRendering());
        _renderEngine->SetSoftwareRendering(_settings.SoftwareRendering());

        switch (_settings.AntialiasingMode())
        {
        case TextAntialiasingMode::Cleartype:
            _renderEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            break;
        case TextAntialiasingMode::Aliased:
            _renderEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
            break;
        case TextAntialiasingMode::Grayscale:
        default:
            _renderEngine->SetAntialiasingMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            break;
        }

        // Refresh our font with the renderer
        const auto actualFontOldSize = _actualFont.GetSize();
        _UpdateFont();
        const auto actualFontNewSize = _actualFont.GetSize();
        if (actualFontNewSize != actualFontOldSize)
        {
            _RefreshSizeUnderLock();
        }
    }

    // Method Description:
    // - Update the font with the renderer. This will be called either when the
    //      font changes or the DPI changes, as DPI changes will necessitate a
    //      font change. This method will *not* change the buffer/viewport size
    //      to account for the new glyph dimensions. Callers should make sure to
    //      appropriately call _DoResizeUnderLock after this method is called.
    // - The write lock should be held when calling this method.
    // Arguments:
    // - initialUpdate: whether this font update should be considered as being
    //   concerned with initialization process. Value forwarded to event handler.
    void ControlCore::_UpdateFont(const bool /*initialUpdate*/)
    {
        const int newDpi = static_cast<int>(static_cast<double>(USER_DEFAULT_SCREEN_DPI) *
                                            _compositionScaleX);

        // !TODO!: MSFT:20895307 If the font doesn't exist, this doesn't
        //      actually fail. We need a way to gracefully fallback.
        _renderer->TriggerFontChange(newDpi, _desiredFont, _actualFont);

        // If the actual font isn't what was requested...
        if (_actualFont.GetFaceName() != _desiredFont.GetFaceName())
        {
            // !TODO!: We _DO_ want this
            // // Then warn the user that we picked something because we couldn't find their font.
            // // Format message with user's choice of font and the font that was chosen instead.
            // const winrt::hstring message{ fmt::format(std::wstring_view{ RS_(L"NoticeFontNotFound") }, _desiredFont.GetFaceName(), _actualFont.GetFaceName()) };
            // // Capture what we need to resume later.
            // [strongThis = get_strong(), message]() -> winrt::fire_and_forget {
            //     // Take these out of the lambda and store them locally
            //     // because the coroutine will lose them into space
            //     // by the time it resumes.
            //     const auto msg = message;
            //     const auto strong = strongThis;

            //     // Pop the rest of this function to the tail of the UI thread
            //     // Just in case someone was holding a lock when they called us and
            //     // the handlers decide to do something that take another lock
            //     // (like ShellExecute pumping our messaging thread...GH#7994)
            //     co_await strong->Dispatcher();

            //     auto noticeArgs = winrt::make<NoticeEventArgs>(NoticeLevel::Warning, std::move(msg));
            //     strong->_raiseNoticeHandlers(*strong, std::move(noticeArgs));
            // }();
        }

        // !TODO!: We _DO_ want this
        // const auto actualNewSize = _actualFont.GetSize();
        // _fontSizeChangedHandlers(actualNewSize.X, actualNewSize.Y, initialUpdate);
    }

    // Method Description:
    // - Set the font size of the terminal control.
    // Arguments:
    // - fontSize: The size of the font.
    void ControlCore::_SetFontSize(int fontSize)
    {
        try
        {
            // Make sure we have a non-zero font size
            const auto newSize = std::max<short>(gsl::narrow_cast<short>(fontSize), 1);
            const auto fontFace = _settings.FontFace();
            const auto fontWeight = _settings.FontWeight();
            _actualFont = { fontFace, 0, fontWeight.Weight, { 0, newSize }, CP_UTF8, false };
            _desiredFont = { _actualFont };

            auto lock = _terminal->LockForWriting();

            // Refresh our font with the renderer
            _UpdateFont();

            // Resize the terminal's BUFFER to match the new font size. This does
            // NOT change the size of the window, because that can lead to more
            // problems (like what happens when you change the font size while the
            // window is maximized?)
            _RefreshSizeUnderLock();
        }
        CATCH_LOG();
    }

    // Method Description:
    // - Reset the font size of the terminal to its default size.
    // Arguments:
    // - none
    void ControlCore::ResetFontSize()
    {
        _SetFontSize(_settings.FontSize());
    }

    // Method Description:
    // - Adjust the font size of the terminal control.
    // Arguments:
    // - fontSizeDelta: The amount to increase or decrease the font size by.
    void ControlCore::AdjustFontSize(int fontSizeDelta)
    {
        const auto newSize = _desiredFont.GetEngineSize().Y + fontSizeDelta;
        _SetFontSize(newSize);
    }

    // Method Description:
    // - Perform a resize for the current size of the swapchainpanel. If the
    //   font size changed, we'll need to resize the buffer to fit the existing
    //   swapchain size. This helper will call _DoResizeUnderLock with the
    //   current size of the swapchain, accounting for scaling due to DPI.
    // - Note that a DPI change will also trigger a font size change, and will
    //   call into here.
    // - The write lock should be held when calling this method, we might be
    //   changing the buffer size in _DoResizeUnderLock.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void ControlCore::_RefreshSizeUnderLock()
    {
        // const auto currentScaleX = SwapChainPanel().CompositionScaleX();
        // const auto currentScaleY = SwapChainPanel().CompositionScaleY();
        // const auto actualWidth = SwapChainPanel().ActualWidth();
        // const auto actualHeight = SwapChainPanel().ActualHeight();

        const auto widthInPixels = _panelWidth * _compositionScaleX;
        const auto heightInPixels = _panelHeight * _compositionScaleY;

        _DoResizeUnderLock(widthInPixels, heightInPixels);
    }

    // Method Description:
    // - Process a resize event that was initiated by the user. This can either
    //   be due to the user resizing the window (causing the swapchain to
    //   resize) or due to the DPI changing (causing us to need to resize the
    //   buffer to match)
    // Arguments:
    // - newWidth: the new width of the swapchain, in pixels.
    // - newHeight: the new height of the swapchain, in pixels.
    void ControlCore::_DoResizeUnderLock(const double newWidth,
                                         const double newHeight)
    {
        SIZE size;
        size.cx = static_cast<long>(newWidth);
        size.cy = static_cast<long>(newHeight);

        // Don't actually resize so small that a single character wouldn't fit
        // in either dimension. The buffer really doesn't like being size 0.
        if (size.cx < _actualFont.GetSize().X || size.cy < _actualFont.GetSize().Y)
        {
            return;
        }

        _terminal->ClearSelection();

        // Tell the dx engine that our window is now the new size.
        THROW_IF_FAILED(_renderEngine->SetWindowSize(size));

        // Invalidate everything
        _renderer->TriggerRedrawAll();

        // Convert our new dimensions to characters
        const auto viewInPixels = Viewport::FromDimensions({ 0, 0 },
                                                           { static_cast<short>(size.cx), static_cast<short>(size.cy) });
        const auto vp = _renderEngine->GetViewportInCharacters(viewInPixels);

        // If this function succeeds with S_FALSE, then the terminal didn't
        // actually change size. No need to notify the connection of this no-op.
        const HRESULT hr = _terminal->UserResize({ vp.Width(), vp.Height() });
        if (SUCCEEDED(hr) && hr != S_FALSE)
        {
            _connection.Resize(vp.Height(), vp.Width());
        }
    }

    void ControlCore::SizeChanged(const double width,
                                  const double height)
    {
        _panelWidth = width;
        _panelHeight = height;

        auto lock = _terminal->LockForWriting();
        const auto currentEngineScale = _renderEngine->GetScaling();

        auto scaledWidth = width * currentEngineScale;
        auto scaledHeight = height * currentEngineScale;
        _DoResizeUnderLock(scaledWidth, scaledHeight);
    }

    void ControlCore::ScaleChanged(const double scaleX,
                                   const double scaleY)
    {
        if (!_renderEngine)
        {
            return;
        }

        _compositionScaleX = scaleX;
        _compositionScaleY = scaleY;

        const auto currentEngineScale = _renderEngine->GetScaling();
        // If we're getting a notification to change to the DPI we already
        // have, then we're probably just beginning the DPI change. Since
        // we'll get _another_ event with the real DPI, do nothing here for
        // now. We'll also skip the next resize in _SwapChainSizeChanged.
        const bool dpiWasUnchanged = currentEngineScale == scaleX;
        if (dpiWasUnchanged)
        {
            return;
        }

        const auto dpi = (float)(scaleX * USER_DEFAULT_SCREEN_DPI);

        const auto actualFontOldSize = _actualFont.GetSize();

        auto lock = _terminal->LockForWriting();

        _renderer->TriggerFontChange(::base::saturated_cast<int>(dpi),
                                     _desiredFont,
                                     _actualFont);

        const auto actualFontNewSize = _actualFont.GetSize();
        if (actualFontNewSize != actualFontOldSize)
        {
            _RefreshSizeUnderLock();
        }
    }

    // !!TODO!! Does the Control really need to ask the Core for this? Or can it
    // suffice with the swapchain value? That's unclear.
    float ControlCore::RendererScale() const
    {
        return _renderEngine->GetScaling();
    }

    void ControlCore::SetSelectionAnchor(Windows::Foundation::Point const& position)
    {
        auto lock = _terminal->LockForWriting();
        _terminal->SetSelectionAnchor(til::point{ til::math::rounding,
                                                  position.X,
                                                  position.Y });
    }

    // Method Description:
    // - Sets selection's end position to match supplied cursor position, e.g. while mouse dragging.
    // Arguments:
    // - ~~cursorPosition: in pixels, relative to the origin of the control~~
    // - cursorPosition: in cells
    void ControlCore::_SetEndSelectionPoint(Windows::Foundation::Point const& position)
    {
        if (!_terminal->IsSelectionActive())
        {
            return;
        }

        // Have to take the lock because the renderer will not draw correctly if
        // you move its endpoints while it is generating a frame.
        auto lock = _terminal->LockForWriting();

        const short lastVisibleRow = std::max<short>(_terminal->GetViewport().Height() - 1, 0);
        const short lastVisibleCol = std::max<short>(_terminal->GetViewport().Width() - 1, 0);

        til::point terminalPosition{ til::math::rounding,
                                     std::clamp<float>(position.X, 0, lastVisibleCol),
                                     std::clamp<float>(position.Y, 0, lastVisibleRow) };

        // save location (for rendering) + render
        _terminal->SetSelectionEnd(terminalPosition);
        _renderer->TriggerSelection();
        // _selectionNeedsToBeCopied = true;
    }

    // Called when the Terminal wants to set something to the clipboard, i.e.
    // when an OSC 52 is emitted.
    void ControlCore::_TerminalCopyToClipboard(const std::wstring_view& wstr)
    {
        auto copyArgs = winrt::make_self<implementation::CopyToClipboardEventArgs>(winrt::hstring(wstr));
        _CopyToClipboardHandlers(*this, *copyArgs);
    }

    // Method Description:
    // - Given a copy-able selection, get the selected text from the buffer and send it to the
    //     Windows Clipboard (CascadiaWin32:main.cpp).
    // - CopyOnSelect does NOT clear the selection
    // Arguments:
    // - singleLine: collapse all of the text to one line
    // - formats: which formats to copy (defined by action's CopyFormatting arg). nullptr
    //             if we should defer which formats are copied to the global setting
    bool ControlCore::CopySelectionToClipboard(bool singleLine,
                                               const Windows::Foundation::IReference<CopyFormat>& formats)
    {
        // no selection --> nothing to copy
        if (!_terminal->IsSelectionActive())
        {
            return false;
        }

        // // Mark the current selection as copied
        // _selectionNeedsToBeCopied = false;

        // extract text from buffer
        const auto bufferData = _terminal->RetrieveSelectedTextFromBuffer(singleLine);

        // convert text: vector<string> --> string
        std::wstring textData;
        for (const auto& text : bufferData.text)
        {
            textData += text;
        }

        // convert text to HTML format
        // GH#5347 - Don't provide a title for the generated HTML, as many
        // web applications will paste the title first, followed by the HTML
        // content, which is unexpected.
        const auto htmlData = formats == nullptr || WI_IsFlagSet(formats.Value(), CopyFormat::HTML) ?
                                  TextBuffer::GenHTML(bufferData,
                                                      _actualFont.GetUnscaledSize().Y,
                                                      _actualFont.GetFaceName(),
                                                      _settings.DefaultBackground()) :
                                  "";

        // convert to RTF format
        const auto rtfData = formats == nullptr || WI_IsFlagSet(formats.Value(), CopyFormat::RTF) ?
                                 TextBuffer::GenRTF(bufferData,
                                                    _actualFont.GetUnscaledSize().Y,
                                                    _actualFont.GetFaceName(),
                                                    _settings.DefaultBackground()) :
                                 "";

        if (!_settings.CopyOnSelect())
        {
            _terminal->ClearSelection();
            _renderer->TriggerSelection();
        }

        // send data up for clipboard
        auto copyArgs = winrt::make_self<CopyToClipboardEventArgs>(winrt::hstring(textData),
                                                                   winrt::to_hstring(htmlData),
                                                                   winrt::to_hstring(rtfData),
                                                                   formats);
        _CopyToClipboardHandlers(*this, *copyArgs);
        return true;
    }

    // Method Description:
    // - Pre-process text pasted (presumably from the clipboard)
    //   before sending it over the terminal's connection.
    void ControlCore::PasteText(const winrt::hstring& hstr)
    {
        _terminal->WritePastedText(hstr);
        _terminal->ClearSelection();
        _terminal->TrySnapOnInput();
    }

    FontInfo ControlCore::GetFont() const
    {
        return _actualFont;
    }

    TerminalConnection::ConnectionState ControlCore::ConnectionState() const
    {
        return _connection.State();
    }

    hstring ControlCore::Title()
    {
        hstring hstr{ _terminal->GetConsoleTitle() };
        return hstr;
    }

    hstring ControlCore::WorkingDirectory() const
    {
        hstring hstr{ _terminal->GetWorkingDirectory() };
        return hstr;
    }

    Windows::Foundation::IReference<winrt::Windows::UI::Color> ControlCore::TabColor() noexcept
    {
        auto coreColor = _terminal->GetTabColor();
        return coreColor.has_value() ? Windows::Foundation::IReference<winrt::Windows::UI::Color>(coreColor.value()) :
                                       nullptr;
    }

    til::color ControlCore::BackgroundColor() const
    {
        return _backgroundColor;
    }

    // Method Description:
    // - Gets the internal taskbar state value
    // Return Value:
    // - The taskbar state of this control
    const size_t ControlCore::TaskbarState() const noexcept
    {
        return _terminal->GetTaskbarState();
    }

    // Method Description:
    // - Gets the internal taskbar progress value
    // Return Value:
    // - The taskbar progress of this control
    const size_t ControlCore::TaskbarProgress() const noexcept
    {
        return _terminal->GetTaskbarProgress();
    }

    int ControlCore::ScrollOffset()
    {
        return _terminal->GetScrollOffset();
    }

    // Function Description:
    // - Gets the height of the terminal in lines of text. This is just the
    //   height of the viewport.
    // Return Value:
    // - The height of the terminal in lines of text
    int ControlCore::ViewHeight() const
    {
        const auto viewPort = _terminal->GetViewport();
        return viewPort.Height();
    }

    // Function Description:
    // - Gets the height of the terminal in lines of text. This includes the
    //   history AND the viewport.
    // Return Value:
    // - The height of the terminal in lines of text
    int ControlCore::BufferHeight() const
    {
        return _terminal->GetViewport().BottomExclusive();
    }

    void ControlCore::_TerminalWarningBell()
    {
        _WarningBellHandlers(*this, nullptr);
    }

    void ControlCore::_TerminalTitleChanged(const std::wstring_view& wstr)
    {
        auto titleArgs = winrt::make_self<TitleChangedEventArgs>(winrt::hstring{ wstr });
        _TitleChangedHandlers(*this, *titleArgs);
    }
    void ControlCore::_TerminalTabColorChanged(const std::optional<til::color> /*color*/)
    {
        _TabColorChangedHandlers(*this, nullptr);
    }

    void ControlCore::_TerminalBackgroundColorChanged(const COLORREF color)
    {
        _backgroundColor = color;
        _BackgroundColorChangedHandlers(*this, nullptr);
    }

    // Method Description:
    // - Update the position and size of the scrollbar to match the given
    //      viewport top, viewport height, and buffer size.
    //   Additionally fires a ScrollPositionChanged event for anyone who's
    //      registered an event handler for us.
    // Arguments:
    // - viewTop: the top of the visible viewport, in rows. 0 indicates the top
    //      of the buffer.
    // - viewHeight: the height of the viewport in rows.
    // - bufferSize: the length of the buffer, in rows
    void ControlCore::_TerminalScrollPositionChanged(const int viewTop,
                                                     const int viewHeight,
                                                     const int bufferSize)
    {
        // !!TODO!! do we need this?
        // // Since this callback fires from non-UI thread, we might be already
        // // closed/closing.
        // if (_closing.load())
        // {
        //     return;
        // }

        // Clear the regex pattern tree so the renderer does not try to render
        // them while scrolling
        _terminal->ClearPatternTree();

        auto scrollArgs = winrt::make_self<ScrollPositionChangedArgs>(viewTop, viewHeight, bufferSize);
        _ScrollPositionChangedHandlers(*this, *scrollArgs);
    }

    void ControlCore::_TerminalCursorPositionChanged()
    {
        _CursorPositionChangedHandlers(*this, nullptr);
    }

    void ControlCore::_TerminalTaskbarProgressChanged()
    {
        _TaskbarProgressChangedHandlers(*this, nullptr);
    }

    bool ControlCore::HasSelection() const
    {
        return _terminal->IsSelectionActive();
    }

    std::vector<std::wstring> ControlCore::SelectedText(bool trimTrailingWhitespace) const
    {
        const auto bufferData = _terminal->RetrieveSelectedTextFromBuffer(trimTrailingWhitespace);
        return bufferData.text;
    }

    ::Microsoft::Console::Types::IUiaData* ControlCore::GetUiaData() const
    {
        return _terminal.get();
    }

    // Method Description:
    // - Search text in text buffer. This is triggered if the user click
    //   search button or press enter.
    // Arguments:
    // - text: the text to search
    // - goForward: boolean that represents if the current search direction is forward
    // - caseSensitive: boolean that represents if the current search is case sensitive
    // Return Value:
    // - <none>
    void ControlCore::Search(const winrt::hstring& text,
                             const bool goForward,
                             const bool caseSensitive)
    {
        // !!TODO!! do we need this?
        if (text.size() == 0 /* || _closing*/)
        {
            return;
        }

        const Search::Direction direction = goForward ?
                                                Search::Direction::Forward :
                                                Search::Direction::Backward;

        const Search::Sensitivity sensitivity = caseSensitive ?
                                                    Search::Sensitivity::CaseSensitive :
                                                    Search::Sensitivity::CaseInsensitive;

        ::Search search(*GetUiaData(), text.c_str(), direction, sensitivity);
        auto lock = _terminal->LockForWriting();
        if (search.FindNext())
        {
            _terminal->SetBlockSelection(false);
            search.Select();
            _renderer->TriggerSelection();
        }
    }

    void ControlCore::SetBackgroundOpacity(const float opacity)
    {
        if (_renderEngine)
        {
            _renderEngine->SetDefaultTextBackgroundOpacity(::base::saturated_cast<float>(opacity));
        }
    }

    // Method Description:
    // - Asynchronously close our connection. The Connection will likely wait
    //   until the attached process terminates before Close returns. If that's
    //   the case, we don't want to block the UI thread waiting on that process
    //   handle.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    winrt::fire_and_forget ControlCore::_AsyncCloseConnection()
    {
        if (auto localConnection{ std::exchange(_connection, nullptr) })
        {
            // Close the connection on the background thread.
            co_await winrt::resume_background();
            localConnection.Close();
            // connection is destroyed.
        }
    }

    void ControlCore::Close()
    {
        // Stop accepting new output and state changes before we disconnect everything.
        _connection.TerminalOutput(_connectionOutputEventToken);
        _connectionStateChangedRevoker.revoke();

        // GH#1996 - Close the connection asynchronously on a background
        // thread.
        // Since TermControlTwo::Close is only ever triggered by the UI, we
        // don't really care to wait for the connection to be completely
        // closed. We can just do it whenever.
        _AsyncCloseConnection();

        {
            // GH#8734:
            // We lock the terminal here to make sure it isn't still being
            // used in the connection thread before we destroy the renderer.
            // However, we must unlock it again prior to triggering the
            // teardown, to avoid the render thread being deadlocked. The
            // renderer may be waiting to acquire the terminal lock, while
            // we're waiting for the renderer to finish.
            auto lock = _terminal->LockForWriting();
        }

        if (auto localRenderEngine{ std::exchange(_renderEngine, nullptr) })
        {
            if (auto localRenderer{ std::exchange(_renderer, nullptr) })
            {
                localRenderer->TriggerTeardown();
                // renderer is destroyed
            }
            // renderEngine is destroyed
        }

        // we don't destroy _terminal here; it now has the same lifetime as the
        // control.
    }

    HANDLE ControlCore::GetSwapChainHandle() const
    {
        return _renderEngine->GetSwapChainHandle();
    }

    void ControlCore::_RendererWarning(const HRESULT hr)
    {
        auto args{ winrt::make_self<RendererWarningArgs>(hr) };
        _RendererWarningHandlers(*this, *args);
    }

    void ControlCore::RenderEngineSwapChainChanged()
    {
        _SwapChainChangedHandlers(*this, nullptr);
    }

    void ControlCore::BlinkAttributeTick()
    {
        auto lock = _terminal->LockForWriting();

        auto& renderTarget = *_renderer;
        auto& blinkingState = _terminal->GetBlinkingState();
        blinkingState.ToggleBlinkingRendition(renderTarget);
    }

    void ControlCore::BlinkCursor()
    {
        if (!_terminal->IsCursorBlinkingAllowed() &&
            _terminal->IsCursorVisible())
        {
            return;
        }
        _terminal->SetCursorOn(!_terminal->IsCursorOn());
    }

    bool ControlCore::CursorOn() const
    {
        return _terminal->IsCursorOn();
    }

    void ControlCore::CursorOn(const bool isCursorOn)
    {
        _terminal->SetCursorOn(isCursorOn);
    }

    void ControlCore::ResumeRendering()
    {
        _renderer->ResetErrorStateAndResume();
    }

    bool ControlCore::IsVtMouseModeEnabled() const
    {
        return _terminal != nullptr && _terminal->IsTrackingMouseInput();
    }

    til::point ControlCore::CursorPosition() const
    {
        // If we haven't been initialized yet, then fake it.
        if (!_initializedTerminal)
        {
            return { 0, 0 };
        }

        auto lock = _terminal->LockForReading();
        return _terminal->GetCursorPosition();
    }

    // This one's really pushing the boundary of "encapsulation". It really
    // belongs in the "Interactivity" layer, which doesn't yet exist. There's so
    // many accesses to the selection in the Core though, that I just put this
    // here. The Control shouldn't be futzing that much with the selection
    // itself.
    void ControlCore::LeftClickOnTerminal(const til::point terminalPosition,
                                          const int numberOfClicks,
                                          const bool altEnabled,
                                          const bool shiftEnabled,
                                          const bool isOnOriginalPosition,
                                          bool& selectionNeedsToBeCopied)
    {
        auto lock = _terminal->LockForWriting();
        // handle ALT key
        _terminal->SetBlockSelection(altEnabled);

        ::Terminal::SelectionExpansionMode mode = ::Terminal::SelectionExpansionMode::Cell;
        if (numberOfClicks == 1)
        {
            mode = ::Terminal::SelectionExpansionMode::Cell;
        }
        else if (numberOfClicks == 2)
        {
            mode = ::Terminal::SelectionExpansionMode::Word;
        }
        else if (numberOfClicks == 3)
        {
            mode = ::Terminal::SelectionExpansionMode::Line;
        }

        // Update the selection appropriately

        // We reset the active selection if one of the conditions apply:
        // - shift is not held
        // - GH#9384: the position is the same as of the first click starting
        //   the selection (we need to reset selection on double-click or
        //   triple-click, so it captures the word or the line, rather than
        //   extending the selection)
        if (HasSelection() && (!shiftEnabled || isOnOriginalPosition))
        {
            // Reset the selection
            _terminal->ClearSelection();
            selectionNeedsToBeCopied = false; // there's no selection, so there's nothing to update
        }

        if (shiftEnabled)
        {
            if (HasSelection())
            {
                // If there is a selection we extend it using the selection mode
                // (expand the "end"selection point)
                _terminal->SetSelectionEnd(terminalPosition, mode);
            }
            else
            {
                // If there is no selection we establish it using the selected mode
                // (expand both "start" and "end" selection points)
                _terminal->MultiClickSelection(terminalPosition, mode);
            }
            selectionNeedsToBeCopied = true;
        }

        _renderer->TriggerSelection();
    }

}