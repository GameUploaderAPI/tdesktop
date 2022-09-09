/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/media/history_view_extended_preview.h"

//#include "history/history_item_components.h"
#include "history/history_item.h"
#include "history/history.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "history/view/media/history_view_media_common.h"
//#include "main/main_session.h"
//#include "main/main_session_settings.h"
#include "media/streaming/media_streaming_utility.h"
#include "ui/effects/spoiler_mess.h"
#include "ui/image/image.h"
#include "ui/chat/chat_style.h"
//#include "ui/cached_round_corners.h"
#include "data/data_session.h"
//#include "data/data_streaming.h"
//#include "data/data_photo.h"
//#include "data/data_photo_media.h"
//#include "data/data_file_click_handler.h"
//#include "data/data_file_origin.h"
//#include "data/data_auto_download.h"
//#include "core/application.h"
#include "payments/payments_checkout_process.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"
#include "core/click_handler_types.h"
#include "styles/style_chat.h"

namespace HistoryView {
namespace {

[[nodiscard]] ClickHandlerPtr MakeInvoiceLink(not_null<HistoryItem*> item) {
	return std::make_shared<LambdaClickHandler>([=](ClickContext context) {
		const auto my = context.other.value<ClickHandlerContext>();
		const auto controller = my.sessionWindow.get();
		Payments::CheckoutProcess::Start(
			item,
			Payments::Mode::Payment,
			(controller
				? crl::guard(
					controller,
					[=](auto) { controller->widget()->activate(); })
				: Fn<void(Payments::CheckoutResult)>()));
	});
}

} // namespace

ExtendedPreview::ExtendedPreview(
	not_null<Element*> parent,
	not_null<Data::Invoice*> invoice)
: Media(parent)
, _invoice(invoice)
, _caption(st::minPhotoSize - st::msgPadding.left() - st::msgPadding.right()) {
	const auto item = parent->data();
	_caption = createCaption(item);
	_link = MakeInvoiceLink(item);
}

ExtendedPreview::~ExtendedPreview() = default;

void ExtendedPreview::ensureThumbnailRead() const {
	if (!_inlineThumbnail.isNull() || _imageCacheInvalid) {
		return;
	}
	const auto &bytes = _invoice->extendedPreview.inlineThumbnailBytes;
	if (bytes.isEmpty()) {
		return;
	}
	_inlineThumbnail = Images::FromInlineBytes(bytes);
	if (_inlineThumbnail.isNull()) {
		_imageCacheInvalid = 1;
	} else {
		history()->owner().registerHeavyViewPart(_parent);
	}
}

bool ExtendedPreview::hasHeavyPart() const {
	return _animation || !_inlineThumbnail.isNull();
}

void ExtendedPreview::unloadHeavyPart() {
	_inlineThumbnail = _imageCache = QImage();
	_animation = nullptr;
	_caption.unloadCustomEmoji();
}

QSize ExtendedPreview::countOptimalSize() {
	if (_parent->media() != this) {
		_caption = Ui::Text::String();
	} else if (_caption.hasSkipBlock()) {
		_caption.updateSkipBlock(
			_parent->skipBlockWidth(),
			_parent->skipBlockHeight());
	}
	const auto &preview = _invoice->extendedPreview;
	const auto dimensions = preview.dimensions;
	const auto minWidth = std::clamp(
		_parent->minWidthForMedia(),
		(_parent->hasBubble()
			? st::historyPhotoBubbleMinWidth
			: st::minPhotoSize),
		st::maxMediaSize);
	const auto scaled = CountDesiredMediaSize(dimensions);
	auto maxWidth = qMax(scaled.width(), minWidth);
	auto minHeight = qMax(scaled.height(), st::minPhotoSize);
	if (preview.videoDuration < 0) {
		accumulate_max(maxWidth, scaled.height());
	}
	if (_parent->hasBubble() && !_caption.isEmpty()) {
		maxWidth = qMax(maxWidth, st::msgPadding.left()
			+ _caption.maxWidth()
			+ st::msgPadding.right());
		minHeight += st::mediaCaptionSkip + _caption.minHeight();
		if (isBubbleBottom()) {
			minHeight += st::msgPadding.bottom();
		}
	}
	return { maxWidth, minHeight };
}

QSize ExtendedPreview::countCurrentSize(int newWidth) {
	const auto &preview = _invoice->extendedPreview;
	const auto dimensions = preview.dimensions;
	const auto minWidth = std::clamp(
		_parent->minWidthForMedia(),
		(_parent->hasBubble()
			? st::historyPhotoBubbleMinWidth
			: st::minPhotoSize),
		std::min(newWidth, st::maxMediaSize));
	const auto scaled = (preview.videoDuration >= 0)
		? CountMediaSize(
			CountDesiredMediaSize(dimensions),
			newWidth)
		: CountPhotoMediaSize(
			CountDesiredMediaSize(dimensions),
			newWidth,
			maxWidth());
	newWidth = qMax(scaled.width(), minWidth);
	auto newHeight = qMax(scaled.height(), st::minPhotoSize);
	if (_parent->hasBubble() && !_caption.isEmpty()) {
		const auto maxWithCaption = qMin(
			st::msgMaxWidth,
			(st::msgPadding.left()
				+ _caption.maxWidth()
				+ st::msgPadding.right()));
		newWidth = qMax(newWidth, maxWithCaption);
		const auto captionw = newWidth
			- st::msgPadding.left()
			- st::msgPadding.right();
		newHeight += st::mediaCaptionSkip + _caption.countHeight(captionw);
		if (isBubbleBottom()) {
			newHeight += st::msgPadding.bottom();
		}
	}
	return { newWidth, newHeight };
}

void ExtendedPreview::draw(Painter &p, const PaintContext &context) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;

	const auto st = context.st;
	const auto sti = context.imageStyle();
	const auto stm = context.messageStyle();
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto bubble = _parent->hasBubble();
	auto captionw = paintw - st::msgPadding.left() - st::msgPadding.right();
	auto rthumb = style::rtlrect(paintx, painty, paintw, painth, width());
	if (bubble) {
		if (!_caption.isEmpty()) {
			painth -= st::mediaCaptionSkip + _caption.countHeight(captionw);
			if (isBubbleBottom()) {
				painth -= st::msgPadding.bottom();
			}
			rthumb = style::rtlrect(paintx, painty, paintw, painth, width());
		}
	} else {
		Ui::FillRoundShadow(p, 0, 0, paintw, painth, sti->msgShadow, sti->msgShadowCorners);
	}
	const auto inWebPage = (_parent->media() != this);
	const auto roundRadius = inWebPage
		? ImageRoundRadius::Small
		: ImageRoundRadius::Large;
	const auto roundCorners = inWebPage ? RectPart::AllCorners : ((isBubbleTop() ? (RectPart::TopLeft | RectPart::TopRight) : RectPart::None)
		| ((isRoundedInBubbleBottom() && _caption.isEmpty()) ? (RectPart::BottomLeft | RectPart::BottomRight) : RectPart::None));
	validateImageCache(rthumb.size(), roundRadius, roundCorners);
	p.drawImage(rthumb.topLeft(), _imageCache);
	fillSpoilerMess(p, context.now, rthumb, roundRadius, roundCorners);
	if (context.selected()) {
		Ui::FillComplexOverlayRect(p, st, rthumb, roundRadius, roundCorners);
	}

	const auto innerSize = st::msgFileLayout.thumbSize;
	QRect inner(rthumb.x() + (rthumb.width() - innerSize) / 2, rthumb.y() + (rthumb.height() - innerSize) / 2, innerSize, innerSize);
	p.setPen(Qt::NoPen);
	if (context.selected()) {
		p.setBrush(st->msgDateImgBgSelected());
	} else {
		const auto over = ClickHandler::showAsActive(_link);
		p.setBrush(over ? st->msgDateImgBgOver() : st->msgDateImgBg());
	}
	{
		PainterHighQualityEnabler hq(p);
		p.drawEllipse(inner);
	}

	// date
	if (!_caption.isEmpty()) {
		p.setPen(stm->historyTextFg);
		_parent->prepareCustomEmojiPaint(p, _caption);
		_caption.draw(p, st::msgPadding.left(), painty + painth + st::mediaCaptionSkip, captionw, style::al_left, 0, -1, context.selection);
	} else if (!inWebPage) {
		auto fullRight = paintx + paintw;
		auto fullBottom = painty + painth;
		if (needInfoDisplay()) {
			_parent->drawInfo(
				p,
				context,
				fullRight,
				fullBottom,
				2 * paintx + paintw,
				InfoDisplayType::Image);
		}
		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()) {
			auto fastShareLeft = (fullRight + st::historyFastShareLeft);
			auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
			_parent->drawRightAction(p, context, fastShareLeft, fastShareTop, 2 * paintx + paintw);
		}
	}
}

void ExtendedPreview::validateImageCache(
		QSize outer,
		ImageRoundRadius radius,
		RectParts corners) const {
	const auto intRadius = static_cast<int>(radius);
	const auto intCorners = static_cast<int>(corners);
	const auto ratio = style::DevicePixelRatio();
	if (_imageCache.size() == (outer * ratio)
		&& _imageCacheRoundRadius == intRadius
		&& _imageCacheRoundCorners == intCorners) {
		return;
	}
	_imageCache = prepareImageCache(outer, radius, corners);
	_imageCacheRoundRadius = intRadius;
	_imageCacheRoundCorners = intCorners;
}

QImage ExtendedPreview::prepareImageCache(
		QSize outer,
		ImageRoundRadius radius,
		RectParts corners) const {
	return Images::Round(prepareImageCache(outer), radius, corners);
}

QImage ExtendedPreview::prepareImageCache(QSize outer) const {
	ensureThumbnailRead();
	return PrepareWithBlurredBackground(outer, {}, {}, _inlineThumbnail);
}

void ExtendedPreview::fillSpoilerMess(
		QPainter &p,
		crl::time now,
		QRect rect,
		ImageRoundRadius radius,
		RectParts corners) const {
	if (!_animation) {
		_animation = std::make_unique<Ui::SpoilerAnimation>([=] {
			_parent->customEmojiRepaint();
		});
		history()->owner().registerHeavyViewPart(_parent);
	}
	_parent->clearCustomEmojiRepaint();
	const auto &spoiler = Ui::DefaultImageSpoiler();
	const auto size = spoiler.canvasSize() / style::DevicePixelRatio();
	const auto frame = spoiler.frame(
		_animation->index(now, _parent->delegate()->elementIsGifPaused()));
	const auto columns = (rect.width() + size - 1) / size;
	const auto rows = (rect.height() + size - 1) / size;
	p.setClipRect(rect);
	p.translate(rect.topLeft());
	for (auto y = 0; y != rows; ++y) {
		for (auto x = 0; x != columns; ++x) {
			p.drawImage(
				QRect(x * size, y * size, size, size),
				*frame.image,
				frame.source);
		}
	}
	p.translate(-rect.topLeft());
	p.setClipping(false);
}

TextState ExtendedPreview::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);

	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) {
		return result;
	}
	auto paintx = 0, painty = 0, paintw = width(), painth = height();
	auto bubble = _parent->hasBubble();

	if (bubble && !_caption.isEmpty()) {
		const auto captionw = paintw
			- st::msgPadding.left()
			- st::msgPadding.right();
		painth -= _caption.countHeight(captionw);
		if (isBubbleBottom()) {
			painth -= st::msgPadding.bottom();
		}
		if (QRect(st::msgPadding.left(), painth, captionw, height() - painth).contains(point)) {
			result = TextState(_parent, _caption.getState(
				point - QPoint(st::msgPadding.left(), painth),
				captionw,
				request.forText()));
			return result;
		}
		painth -= st::mediaCaptionSkip;
	}
	if (QRect(paintx, painty, paintw, painth).contains(point)) {
		result.link = _link;
	}
	if (_caption.isEmpty() && _parent->media() == this) {
		auto fullRight = paintx + paintw;
		auto fullBottom = painty + painth;
		const auto bottomInfoResult = _parent->bottomInfoTextState(
			fullRight,
			fullBottom,
			point,
			InfoDisplayType::Image);
		if (bottomInfoResult.link
			|| bottomInfoResult.cursor != CursorState::None) {
			return bottomInfoResult;
		}
		if (const auto size = bubble ? std::nullopt : _parent->rightActionSize()) {
			auto fastShareLeft = (fullRight + st::historyFastShareLeft);
			auto fastShareTop = (fullBottom - st::historyFastShareBottom - size->height());
			if (QRect(fastShareLeft, fastShareTop, size->width(), size->height()).contains(point)) {
				result.link = _parent->rightActionLink();
			}
		}
	}
	return result;
}

bool ExtendedPreview::toggleSelectionByHandlerClick(const ClickHandlerPtr &p) const {
	return p == _link;
}

bool ExtendedPreview::dragItemByHandler(const ClickHandlerPtr &p) const {
	return p == _link;
}

bool ExtendedPreview::needInfoDisplay() const {
	return _parent->data()->isSending()
		|| _parent->data()->hasFailed()
		|| _parent->isUnderCursor()
		|| _parent->isLastAndSelfMessage();
}

TextForMimeData ExtendedPreview::selectedText(TextSelection selection) const {
	return _caption.toTextForMimeData(selection);
}

bool ExtendedPreview::needsBubble() const {
	if (!_caption.isEmpty()) {
		return true;
	}
	const auto item = _parent->data();
	return !item->isService()
		&& (item->repliesAreComments()
			|| item->externalReply()
			|| item->viaBot()
			|| _parent->displayedReply()
			|| _parent->displayForwardedFrom()
			|| _parent->displayFromName());
}

QPoint ExtendedPreview::resolveCustomInfoRightBottom() const {
	const auto skipx = (st::msgDateImgDelta + st::msgDateImgPadding.x());
	const auto skipy = (st::msgDateImgDelta + st::msgDateImgPadding.y());
	return QPoint(width() - skipx, height() - skipy);
}

void ExtendedPreview::parentTextUpdated() {
	_caption = (_parent->media() == this)
		? createCaption(_parent->data())
		: Ui::Text::String();
	history()->owner().requestViewResize(_parent);
}

} // namespace HistoryView