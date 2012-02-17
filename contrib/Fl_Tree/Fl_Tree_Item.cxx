#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <FL/Fl_Widget.H>
#include <FL/Fl_Tree_Item.H>
#include <FL/Fl_Tree_Prefs.H>

//////////////////////
// Fl_Tree_Item.cxx
//////////////////////
//
// Fl_Tree -- This file is part of the Fl_Tree widget for FLTK
// Copyright (C) 2009 by Greg Ercolano.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Library General Public
// License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Library General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
// USA.
//

// Was the last event inside the specified xywh?
static int event_inside(const int xywh[4]) {
    return(Fl::event_inside(xywh[0],xywh[1],xywh[2],xywh[3]));
}

/// Constructor.
///     Makes a new instance of Fl_Tree_Item using defaults from 'prefs'.
///
Fl_Tree_Item::Fl_Tree_Item(const Fl_Tree_Prefs &prefs) {
    _label        = 0;
    _labelfont    = prefs.labelfont();
    _labelsize    = prefs.labelsize();
    _labelfgcolor = prefs.fgcolor();
    _labelbgcolor = prefs.bgcolor();
    _widget       = 0;
    _open         = 1;
    _visible      = 1;
    _active       = 1;
    _selected     = 0;
    _xywh[0]      = 0;
    _xywh[1]      = 0;
    _xywh[2]      = 0;
    _xywh[3]      = 0;
    _collapse_xywh[0] = 0;
    _collapse_xywh[1] = 0;
    _collapse_xywh[2] = 0;
    _collapse_xywh[3] = 0;
    _label_xywh[0]    = 0;
    _label_xywh[1]    = 0;
    _label_xywh[2]    = 0;
    _label_xywh[3]    = 0;
    _usericon         = 0;
    _userdata         = 0; // GMSH
    _parent           = 0;
}

// DTOR
Fl_Tree_Item::~Fl_Tree_Item() {
    if ( _label ) { 
        free((void*)_label);
	_label = 0;
    }
    _widget = 0;			// Fl_Group will handle destruction
    _usericon = 0;			// user handled allocation
    //_children.clear();		// array's destructor handles itself
}

/// Copy constructor.
Fl_Tree_Item::Fl_Tree_Item(const Fl_Tree_Item *o) {
    _label        = o->label() ? strdup(o->label()) : 0;
    _labelfont    = o->labelfont();
    _labelsize    = o->labelsize();
    _labelfgcolor = o->labelfgcolor();
    _labelbgcolor = o->labelbgcolor();
    _widget       = o->widget();
    _open         = o->_open;
    _visible      = o->_visible;
    _active       = o->_active;
    _selected     = o->_selected;
    _xywh[0]      = o->_xywh[0];
    _xywh[1]      = o->_xywh[1];
    _xywh[2]      = o->_xywh[2];
    _xywh[3]      = o->_xywh[3];
    _collapse_xywh[0] = o->_collapse_xywh[0];
    _collapse_xywh[1] = o->_collapse_xywh[1];
    _collapse_xywh[2] = o->_collapse_xywh[2];
    _collapse_xywh[3] = o->_collapse_xywh[3];
    _label_xywh[0]    = o->_label_xywh[0];
    _label_xywh[1]    = o->_label_xywh[1];
    _label_xywh[2]    = o->_label_xywh[2];
    _label_xywh[3]    = o->_label_xywh[3];
    _usericon         = o->usericon();
    _parent           = o->_parent;
}

/// Print the tree as 'ascii art' to stdout.
/// Used mainly for debugging.
///
void Fl_Tree_Item::show_self(const char *indent) const {
    if ( label() ) {
        printf("%s-%s (%d children, this=%p, parent=%p depth=%d)\n",
	    indent,label(),children(),(void*)this, (void*)_parent, depth());
    }
    if ( children() ) {
	char *i2 = (char*)malloc(strlen(indent) + 2);
	strcpy(i2, indent);
	strcat(i2, " |");
	for ( int t=0; t<children(); t++ ) {
	    child(t)->show_self(i2);
	}
    }
    fflush(stdout);
}

/// Set the label. Makes a copy of the name.
void Fl_Tree_Item::label(const char *name) {
    if ( _label ) { free((void*)_label); _label = 0; }
    _label = name ? strdup(name) : 0;
}

/// Return the label.
const char *Fl_Tree_Item::label() const {
    return(_label);
}

/// Return child item for the specified 'index'.
const Fl_Tree_Item *Fl_Tree_Item::child(int index) const {
    return(_children[index]);
}

/// Clear all the children for this item.
void Fl_Tree_Item::clear_children() {
    _children.clear();
}

/// Return the index of the immediate child of this item that has the label 'name'.
///
/// \returns index of found item, or -1 if not found.
///
int Fl_Tree_Item::find_child(const char *name) {
    for ( int t=0; t<children(); t++ ) {
        if ( child(t)->label() ) {
	    if ( strcmp(child(t)->label(), name) == 0 ) {
		return(t);
	    }
	}
    }
    return(-1);
}

/// Find item by descending array of names.
/// Only Fl_Tree should need this method.
///
/// \returns item, or 0 if not found
///
const Fl_Tree_Item *Fl_Tree_Item::find_item(char **arr) const {
    for ( int t=0; t<children(); t++ ) {
        if ( child(t)->label() ) {
	    if ( strcmp(child(t)->label(), *arr) == 0 ) {	// match?
	        if ( *(arr+1) ) {				// more in arr? descend
		    return(_children[t]->find_item(arr+1));
		} else {					// end of arr? done
		    return(_children[t]);
		}
	    }
	}
    }
    return(0);
}

/// Find item by by descending array of names.
/// Only Fl_Tree should need this method.
///
/// \returns item, or 0 if not found
///
Fl_Tree_Item *Fl_Tree_Item::find_item(char **arr) {
    for ( int t=0; t<children(); t++ ) {
        if ( child(t)->label() ) {
	    if ( strcmp(child(t)->label(), *arr) == 0 ) {	// match?
	        if ( *(arr+1) ) {				// more in arr? descend
		    return(_children[t]->find_item(arr+1));
		} else {					// end of arr? done
		    return(_children[t]);
		}
	    }
	}
    }
    return(0);
}

/// Find the index number for the specified 'item'
/// in the current item's list of children.
///
/// \returns the index, or -1 if not found.
///
int Fl_Tree_Item::find_child(Fl_Tree_Item *item) {
    for ( int t=0; t<children(); t++ ) {
	if ( item == child(t) ) {
	    return(t);
	}
    }
    return(-1);
}

/// Add a new child to this item with the name 'new_label', with defaults from 'prefs'.
/// An internally managed copy is made of the label string.
/// Adds the item based on the value of prefs.sortorder().
///
Fl_Tree_Item *Fl_Tree_Item::add(const Fl_Tree_Prefs &prefs, const char *new_label) {
    Fl_Tree_Item *item = new Fl_Tree_Item(prefs);
    item->label(new_label);
    item->_parent = this;
    switch ( prefs.sortorder() ) {
        case FL_TREE_SORT_NONE: {
	    _children.add(item);
	    return(item);
	}
        case FL_TREE_SORT_ASCENDING: {
	    for ( int t=0; t<_children.total(); t++ ) {
	        Fl_Tree_Item *c = _children[t];
	        if ( c->label() && strcmp(c->label(), new_label) > 0 ) {
		    _children.insert(t, item);
		    return(item);
		}
	    }
	    _children.add(item);
	    return(item);
	}
        case FL_TREE_SORT_DESCENDING: {
	    for ( int t=0; t<_children.total(); t++ ) {
	        Fl_Tree_Item *c = _children[t];
	        if ( c->label() && strcmp(c->label(), new_label) < 0 ) {
		    _children.insert(t, item);
		    return(item);
		}
	    }
	    _children.add(item);
	    return(item);
	}
    }
    return(item);
}

/// Add a child recursively into tree.
/// Should be used only by Fl_Tree's internals.
/// Adds the item based on the value of prefs.sortorder().
///
Fl_Tree_Item *Fl_Tree_Item::add(const Fl_Tree_Prefs &prefs, char **arr) {
    int t = find_child(*arr);
    Fl_Tree_Item *item;
    if ( t == -1 ) {
        item = (Fl_Tree_Item*)add(prefs, *arr);
    } else {
        item = (Fl_Tree_Item*)child(t);
    }
    if ( *(arr+1) ) {		// descend?
        return(item->add(prefs, arr+1));
    } else {
        return(item);		// end? done
    }
}

/// Insert a new item into current item's children at a specified position.
Fl_Tree_Item *Fl_Tree_Item::insert(const Fl_Tree_Prefs &prefs, const char *new_label, int pos) {
    Fl_Tree_Item *item = new Fl_Tree_Item(prefs);
    item->label(new_label);
    item->_parent = this;
    _children.insert(pos, item);
    return(item);
}

/// Insert a new item above this item.
Fl_Tree_Item *Fl_Tree_Item::insert_above(const Fl_Tree_Prefs &prefs, const char *new_label) {
    Fl_Tree_Item *p = _parent;
    if ( ! p ) return(0);
    // Walk our parent's children to find ourself
    for ( int t=0; t<p->children(); t++ ) {
        Fl_Tree_Item *c = p->child(t);
	if ( this == c ) {
	    return(p->insert(prefs, new_label, t));
	}
    }
    return(0);
}

/// Remove child by item.
///    Returns 0 if removed, -1 if item not an immediate child.
///
int Fl_Tree_Item::remove_child(Fl_Tree_Item *item) {
    for ( int t=0; t<children(); t++ ) {
        if ( child(t) == item ) {
	    item->clear_children();
	    _children.remove(t);
	    return(0);
	}
    }
    return(-1);
}

/// Remove immediate child (and its children) by its label 'name'.
///    Returns 0 if removed, -1 if not found.
///
int Fl_Tree_Item::remove_child(const char *name) {
    for ( int t=0; t<children(); t++ ) {
        if ( child(t)->label() ) {
	    if ( strcmp(child(t)->label(), name) == 0 ) {
	        _children.remove(t);
		return(0);
	    }
	}
    }
    return(-1);
}

/// Swap two of our children, given two child index values.
/// Use this eg. for sorting.
///
/// This method is FAST, and does not involve lookups.
///
/// No range checking is done on either index value.
///
/// \returns
///    -    0 : OK
///    -   -1 : failed: 'a' or 'b' is not our immediate child
///
void Fl_Tree_Item::swap_children(int ax, int bx) {
    _children.swap(ax, bx);
}

/// Swap two of our children, given item pointers.
/// Use this eg. for sorting. 
///
/// This method is SLOW because it involves linear lookups.
/// For speed, use swap_children(int,int) instead.
///
/// \returns
///    -    0 : OK
///    -   -1 : failed: 'a' or 'b' is not our immediate child
///
int Fl_Tree_Item::swap_children(Fl_Tree_Item *a, Fl_Tree_Item *b) {
    int ax = -1, bx = -1;
    for ( int t=0; t<children(); t++ ) {	// find index for a and b
        if ( _children[t] == a ) { ax = t; if ( bx != -1 ) break; else continue; }
	if ( _children[t] == b ) { bx = t; if ( ax != -1 ) break; else continue; }
    }
    if ( ax == -1 || bx == -1 ) return(-1);	// not found? fail
    swap_children(ax,bx);
    return(0);
}

/// Internal: Horizontal connector line based on preference settings.
void Fl_Tree_Item::draw_horizontal_connector(int x1, int x2, int y, const Fl_Tree_Prefs &prefs) {
    fl_color(prefs.connectorcolor());
    switch ( prefs.connectorstyle() ) {
        case FL_TREE_CONNECTOR_SOLID:
	    y |= 1;				// force alignment w/dot pattern
	    fl_line(x1,y,x2,y);
	    return;
        case FL_TREE_CONNECTOR_DOTTED:
	    y |= 1;				// force alignment w/dot pattern
	    for ( int xx=x1; xx<=x2; xx++ ) {
		if ( !(xx & 1) ) fl_point(xx, y);
	    }
	    return;
        case FL_TREE_CONNECTOR_NONE:
	    return;
    }
}

/// Internal: Vertical connector line based on preference settings.
void Fl_Tree_Item::draw_vertical_connector(int x, int y1, int y2, const Fl_Tree_Prefs &prefs) {
    fl_color(prefs.connectorcolor());
    switch ( prefs.connectorstyle() ) {
        case FL_TREE_CONNECTOR_SOLID:
	    y1 |= 1;				// force alignment w/dot pattern
	    y2 |= 1;				// force alignment w/dot pattern
	    fl_line(x,y1,x,y2);
	    return;
        case FL_TREE_CONNECTOR_DOTTED:
	    y1 |= 1;				// force alignment w/dot pattern
	    y2 |= 1;				// force alignment w/dot pattern
	    for ( int yy=y1; yy<=y2; yy++ ) {
		if ( yy & 1 ) fl_point(x, yy);
	    }
	    return;
        case FL_TREE_CONNECTOR_NONE:
	    return;
    }
}

/// Find the item that the last event was over.
///
///    Returns the item if its visible, and mouse is over it.
///    Works even if widget deactivated.
///    Use event_on_collapse_icon() to determine if collapse button was pressed.
///
///    \returns const visible item under the event if found, or 0 if none.
///
const Fl_Tree_Item *Fl_Tree_Item::find_clicked() const {
    if ( ! _visible ) return(0);
    if ( event_inside(_xywh) ) {		// event within this item?
	return(this);				// found
    }
    if ( is_open() ) {				// open? check children of this item
	for ( int t=0; t<children(); t++ ) {
	    const Fl_Tree_Item *item;
	    if ( ( item = _children[t]->find_clicked() ) != NULL) {	// check child and its descendents
	        return(item);						// found?
	    }
	}
    }
    return(0);
}

/// Non-const version of the above.
/// Find the item that the last event was over.
///
///    Returns the item if its visible, and mouse is over it.
///    Works even if widget deactivated.
///    Use event_on_collapse_icon() to determine if collapse button was pressed.
///
///    \returns the visible item under the event if found, or 0 if none.
///
Fl_Tree_Item *Fl_Tree_Item::find_clicked() {
    if ( ! _visible ) return(0);
    if ( event_inside(_xywh) ) {		// event within this item?
	return(this);				// found
    }
    if ( is_open() ) {				// open? check children of this item
	for ( int t=0; t<children(); t++ ) {
	    Fl_Tree_Item *item;
	    if ( ( item = _children[t]->find_clicked() ) != NULL ) {	// check child and its descendents
	        return(item);						// found?
	    }
	}
    }
    return(0);
}

/// Draw this item and its children.
void Fl_Tree_Item::draw(int X, int &Y, int W, Fl_Widget *tree, 
                        const Fl_Tree_Prefs &prefs, int lastchild) {
    if ( ! _visible ) return; 
    fl_font(_labelfont, _labelsize);
    int H = _labelsize + fl_descent() + prefs.linespacing();
    // Colors, fonts
    Fl_Color fg = _selected ? prefs.bgcolor()     : _labelfgcolor;
    Fl_Color bg = _selected ? prefs.selectcolor() : _labelbgcolor;
    if ( ! _active ) {
	fg = fl_inactive(fg);
	if ( _selected ) bg = fl_inactive(bg);
    }
    // Update the xywh of this item
    _xywh[0] = X;
    _xywh[1] = Y;
    _xywh[2] = W;
    _xywh[3] = H;
    // Text size
    int textw=0, texth=0;
    fl_measure(_label, textw, texth, 0);
    int textycenter = Y+(H/2);
    int &icon_x = _collapse_xywh[0] = X-1;
    int &icon_y = _collapse_xywh[1] = textycenter - (prefs.openicon()->h()/2);
    int &icon_w = _collapse_xywh[2] = prefs.openicon()->w();
    _collapse_xywh[3] = prefs.openicon()->h();
    // Horizontal connector values
    int hstartx  = X+icon_w/2-1;
    int hendx    = hstartx + prefs.connectorwidth();
    int hcenterx = X + icon_w + ((hendx - (X + icon_w)) / 2);

    // See if we should draw this item
    //    If this item is root, and showroot() is disabled, don't draw.
    //
    char drawthis = ( is_root() && prefs.showroot() == 0 ) ? 0 : 1;
    if ( drawthis ) {
	// Draw connectors
	if ( prefs.connectorstyle() != FL_TREE_CONNECTOR_NONE ) {
	    // Horiz connector between center of icon and text
	    draw_horizontal_connector(hstartx, hendx, textycenter, prefs);
	    if ( has_children() && is_open() ) {
		// Small vertical line down to children
		draw_vertical_connector(hcenterx, textycenter, Y+H, prefs);
	    }
	    // Connectors for last child
	    if ( ! is_root() ) {
		if ( lastchild ) {
		    draw_vertical_connector(hstartx, Y, textycenter, prefs);
		} else {
		    draw_vertical_connector(hstartx, Y, Y+H, prefs);
		}
	    }
	} 
	// Draw collapse icon
	if ( has_children() && prefs.showcollapse() ) {
	    // Draw icon image
	    if ( is_open() ) {
		prefs.closeicon()->draw(icon_x,icon_y);
	    } else {
		prefs.openicon()->draw(icon_x,icon_y);
	    }
	}
	// Background for this item
	int &bx = _label_xywh[0] = X+(icon_w/2-1+prefs.connectorwidth());
	int &by = _label_xywh[1] = Y;
	int &bw = _label_xywh[2] = W-(icon_w/2-1+prefs.connectorwidth());
	int &bh = _label_xywh[3] = texth;
	// Draw bg only if different from tree's bg
	if ( bg != tree->color() || is_selected() ) {
	    if ( is_selected() ) {
		// Selected? Use selectbox() style
		fl_draw_box(prefs.selectbox(), bx, by, bw, bh, bg);
	    } else {
		// Not Selected? use plain filled rectangle
		fl_color(bg);
		fl_rectf(bx, by, bw, bh);
	    }
	}
	// Draw user icon (if any)
	int useroff = (icon_w/2-1+prefs.connectorwidth());
	if ( usericon() ) {
	    // Item has user icon? Use it
	    useroff += prefs.usericonmarginleft();
	    usericon()->draw(X+useroff,icon_y);
	    useroff += usericon()->w();
	} else if ( prefs.usericon() ) {
	    // Prefs has user icon? Use it
	    useroff += prefs.usericonmarginleft();
	    prefs.usericon()->draw(X+useroff,icon_y);
	    useroff += prefs.usericon()->w();
	}
	useroff += prefs.labelmarginleft();
	// Draw label
	if ( widget() ) {
	    // Widget? Draw it
	    int lx = X+useroff;
	    int ly = by;
	    int lw = widget()->w();
	    int lh = bh;
	    if ( widget()->x() != lx || widget()->y() != ly ||
		 widget()->w() != lw || widget()->h() != lh ) {
		widget()->resize(lx, ly, lw, lh);		// fltk will handle drawing this
	    }
	} else {
	    // No label widget? Draw text label
	    if ( _label ) {
		fl_color(fg);
		fl_draw(_label, X+useroff, Y+H-fl_descent()-1);
	    }
	}
	Y += H;
    }			// end drawthis
    // Draw children
    if ( has_children() && is_open() ) {
	int child_x = drawthis ? 			// offset children to right,
	              (hcenterx - (icon_w/2) + 1) : X;	// unless didn't drawthis
	int child_w = W - (child_x-X);
	int child_y_start = Y;
	for ( int t=0; t<children(); t++ ) {
	    int lastchild = ((t+1)==children()) ? 1 : 0;
	    _children[t]->draw(child_x, Y, child_w, tree, prefs, lastchild);
	}
	if ( ! lastchild ) {
	    draw_vertical_connector(hstartx, child_y_start, Y, prefs);
	}
    }
}

/// Was the event on the 'collapse' button?
///
int Fl_Tree_Item::event_on_collapse_icon(const Fl_Tree_Prefs &prefs) const {
    if ( _visible && _active && has_children() && prefs.showcollapse() ) {
        return(event_inside(_collapse_xywh) ? 1 : 0);
    } else {
	return(0);
    }
}

/// Was event on the label()?
///
int Fl_Tree_Item::event_on_label(const Fl_Tree_Prefs &prefs) const {
    if ( _visible && _active ) {
        return(event_inside(_label_xywh) ? 1 : 0);
    } else {
	return(0);
    }
}

/// Internal: Show the FLTK widget() for this item and all children.
/// Used by open() to re-show widgets that were hidden by a previous close()
///
void Fl_Tree_Item::show_widgets() {
    if ( _widget ) _widget->show();
    if ( is_open() ) {
	for ( int t=0; t<_children.total(); t++ ) {
	    _children[t]->show_widgets();
	}
    }
}

/// Internal: Hide the FLTK widget() for this item and all children.
/// Used by close() to hide widgets.
///
void Fl_Tree_Item::hide_widgets() {
    if ( _widget ) _widget->hide();
    for ( int t=0; t<_children.total(); t++ ) {
	_children[t]->hide_widgets();
    }
}

/// Open this item and all its children.
void Fl_Tree_Item::open() {
    _open = 1;
    // Tell children to show() their widgets
    for ( int t=0; t<_children.total(); t++ ) {
        _children[t]->show_widgets();
    }
}

/// Close this item and all its children.
void Fl_Tree_Item::close() {
    _open = 0;
    // Tell children to hide() their widgets
    for ( int t=0; t<_children.total(); t++ ) {
        _children[t]->hide_widgets();
    }
}

/// Returns how many levels deep this item is in the hierarchy.
///
/// For instance; root has a depth of zero, and its immediate children
/// would have a depth of 1, and so on.
///
int Fl_Tree_Item::depth() const {
    int count = 0;
    const Fl_Tree_Item *item = parent();
    while ( item ) {
	++count;
	item = item->parent();
    }
    return(count);
}

/// Return the next item in the tree.
///
/// This method can be used to walk the tree forward.
/// For an example of how to use this method, see Fl_Tree::first().
/// 
/// \returns the next item in the tree, or 0 if there's no more items.
///
Fl_Tree_Item *Fl_Tree_Item::next() {
    Fl_Tree_Item *p, *c = this;
    if ( c->has_children() ) {
	return(c->child(0));
    }
    while ( ( p = c->parent() ) != NULL ) {	// loop upwards through parents
	int t = p->find_child(c);		// find our position in parent's children[] array
	if ( ++t < p->children() )		// not last child?
	    return(p->child(t));		// return next child
	c = p;					// child becomes parent to move up generation
    }						// loop: moves up to next parent
    return(0);					// hit root? done
}

/// Return the previous item in the tree.
///
/// This method can be used to walk the tree backwards.
/// For an example of how to use this method, see Fl_Tree::last().
/// 
/// \returns the previous item in the tree, or 0 if there's no item above this one (hit the root).
///
Fl_Tree_Item *Fl_Tree_Item::prev() {
    Fl_Tree_Item *p=parent();		// start with parent
    if ( ! p ) return(0);		// hit root? done
    int t = p->find_child(this);	// find our position in parent's children[] array
    if ( --t == -1 ) {	 		// are we first child?
	return(p);			// return immediate parent
    }
    p = p->child(t);			// take parent's previous child
    while ( p->has_children() ) {	// has children?
	p = p->child(p->children()-1);	// take last child
    }
    return(p);
}
