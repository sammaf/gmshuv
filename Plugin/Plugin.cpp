// $Id: Plugin.cpp,v 1.92 2007-09-24 08:14:30 geuzaine Exp $
//
// Copyright (C) 1997-2007 C. Geuzaine, J.-F. Remacle
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA.
// 
// Please report all bugs and problems to <gmsh@geuz.org>.

#include "Plugin.h"

PView *GMSH_Post_Plugin::getView(int index, PView *view)
{
  if(index < 0)
    index = view ? view->getIndex() : 0;

  if(index >= 0 && index < (int)PView::list.size()){
    return PView::list[index];
  }
  else{
    Msg(GERROR, "View[%d] does not exist", index);
    return 0;
  }
}

PViewDataList *GMSH_Post_Plugin::getDataList(PView *view)
{
  if(!view) return 0;

  PViewDataList *data = dynamic_cast<PViewDataList*>(view->getData());
  if(data){
    return data;
  }
  else{
    // FIXME: do automatic data conversion here
    Msg(GERROR, "This plugin can only be run on list-based datasets");
    return 0;
  }
}
