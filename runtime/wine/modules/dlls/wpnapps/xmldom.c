/*
 * Windows.Data.Xml.Dom over msxml's DOM
 *
 * One object stands for any node, and answers for the interfaces its type
 * has: a document, an element, text, an attribute. Node lists are msxml's
 * lists seen as IVectorView<IXmlNode>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(toast);

static HRESULT hstring_from_bstr( BSTR str, HSTRING *out )
{
    return WindowsCreateString( str, str ? SysStringLen( str ) : 0, out );
}

static BSTR bstr_from_hstring( HSTRING str )
{
    UINT32 len;
    const WCHAR *buffer = WindowsGetStringRawBuffer( str, &len );
    return SysAllocStringLen( buffer, len );
}

/* a string as the IInspectable a node's value is: an IPropertyValue */
static HRESULT box_string( BSTR str, IInspectable **out )
{
    IPropertyValueStatics *statics;
    HSTRING name, value;
    HRESULT hr;

    *out = NULL;
    if (!str) return S_OK;
    if (FAILED(hr = WindowsCreateString( RuntimeClass_Windows_Foundation_PropertyValue,
                                         wcslen( RuntimeClass_Windows_Foundation_PropertyValue ), &name )))
        return hr;
    hr = RoGetActivationFactory( name, &IID_IPropertyValueStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = hstring_from_bstr( str, &value )))
    {
        hr = IPropertyValueStatics_CreateString( statics, value, out );
        WindowsDeleteString( value );
    }
    IPropertyValueStatics_Release( statics );
    return hr;
}

static BSTR unbox_string( IInspectable *value )
{
    IPropertyValue *property;
    HSTRING str;
    BSTR ret = NULL;

    if (!value || FAILED(IInspectable_QueryInterface( value, &IID_IPropertyValue, (void **)&property ))) return NULL;
    if (SUCCEEDED(IPropertyValue_GetString( property, &str )))
    {
        ret = bstr_from_hstring( str );
        WindowsDeleteString( str );
    }
    IPropertyValue_Release( property );
    return ret;
}

struct xml_node
{
    IXmlNode IXmlNode_iface;
    IXmlNodeSelector IXmlNodeSelector_iface;
    IXmlNodeSerializer IXmlNodeSerializer_iface;
    IXmlDocument IXmlDocument_iface;
    IXmlDocumentIO IXmlDocumentIO_iface;
    IXmlElement IXmlElement_iface;
    IXmlCharacterData IXmlCharacterData_iface;
    IXmlText IXmlText_iface;
    IXmlAttribute IXmlAttribute_iface;
    LONG ref;

    IXMLDOMNode *node;
    DOMNodeType type;
};

static const struct IXmlNodeVtbl xml_node_vtbl;

static HRESULT wrap_node( IXMLDOMNode *node, REFIID iid, void **out );
static HRESULT wrap_list( IXMLDOMNodeList *list, IXmlNodeList **out );

/* msxml's node under one of ours, whichever of its interfaces it came as */
IXMLDOMNode *xml_node_unwrap( IUnknown *unknown )
{
    IXmlNode *node;
    IXMLDOMNode *ret = NULL;

    if (!unknown || FAILED(IUnknown_QueryInterface( unknown, &IID_IXmlNode, (void **)&node ))) return NULL;
    if (node->lpVtbl == &xml_node_vtbl) ret = CONTAINING_RECORD( node, struct xml_node, IXmlNode_iface )->node;
    IXmlNode_Release( node );
    return ret;
}

/* msxml answers S_FALSE with nothing; Windows answers S_OK with NULL */
static HRESULT wrap_result( HRESULT hr, IXMLDOMNode *node, REFIID iid, void **out )
{
    *out = NULL;
    if (FAILED(hr)) return hr;
    if (!node) return S_OK;
    hr = wrap_node( node, iid, out );
    IXMLDOMNode_Release( node );
    return hr;
}

static HRESULT wrap_list_result( HRESULT hr, IXMLDOMNodeList *list, IXmlNodeList **out )
{
    *out = NULL;
    if (FAILED(hr)) return hr;
    if (!list) return S_OK;
    hr = wrap_list( list, out );
    IXMLDOMNodeList_Release( list );
    return hr;
}

static inline struct xml_node *impl_from_IXmlNode( IXmlNode *iface )
{
    return CONTAINING_RECORD( iface, struct xml_node, IXmlNode_iface );
}

static HRESULT WINAPI xml_node_QueryInterface( IXmlNode *iface, REFIID iid, void **out )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IXmlNode ))
        *out = &impl->IXmlNode_iface;
    else if (IsEqualGUID( iid, &IID_IXmlNodeSelector ))
        *out = &impl->IXmlNodeSelector_iface;
    else if (IsEqualGUID( iid, &IID_IXmlNodeSerializer ))
        *out = &impl->IXmlNodeSerializer_iface;
    else if (impl->type == NODE_DOCUMENT && IsEqualGUID( iid, &IID_IXmlDocument ))
        *out = &impl->IXmlDocument_iface;
    else if (impl->type == NODE_DOCUMENT && IsEqualGUID( iid, &IID_IXmlDocumentIO ))
        *out = &impl->IXmlDocumentIO_iface;
    else if (impl->type == NODE_ELEMENT && IsEqualGUID( iid, &IID_IXmlElement ))
        *out = &impl->IXmlElement_iface;
    else if ((impl->type == NODE_TEXT || impl->type == NODE_CDATA_SECTION || impl->type == NODE_COMMENT) &&
             IsEqualGUID( iid, &IID_IXmlCharacterData ))
        *out = &impl->IXmlCharacterData_iface;
    else if ((impl->type == NODE_TEXT || impl->type == NODE_CDATA_SECTION) && IsEqualGUID( iid, &IID_IXmlText ))
        *out = &impl->IXmlText_iface;
    else if (impl->type == NODE_ATTRIBUTE && IsEqualGUID( iid, &IID_IXmlAttribute ))
        *out = &impl->IXmlAttribute_iface;

    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI xml_node_AddRef( IXmlNode *iface )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI xml_node_Release( IXmlNode *iface )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        IXMLDOMNode_Release( impl->node );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI xml_node_GetIids( IXmlNode *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_node_GetRuntimeClassName( IXmlNode *iface, HSTRING *class_name )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    const WCHAR *name;

    switch (impl->type)
    {
    case NODE_DOCUMENT:  name = RuntimeClass_Windows_Data_Xml_Dom_XmlDocument; break;
    case NODE_ELEMENT:   name = RuntimeClass_Windows_Data_Xml_Dom_XmlElement; break;
    case NODE_TEXT:      name = RuntimeClass_Windows_Data_Xml_Dom_XmlText; break;
    case NODE_ATTRIBUTE: name = RuntimeClass_Windows_Data_Xml_Dom_XmlAttribute; break;
    case NODE_COMMENT:   name = RuntimeClass_Windows_Data_Xml_Dom_XmlComment; break;
    default:             return E_NOTIMPL;
    }
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI xml_node_GetTrustLevel( IXmlNode *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI xml_node_get_NodeValue( IXmlNode *iface, IInspectable **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    VARIANT var;
    HRESULT hr;

    *value = NULL;
    VariantInit( &var );
    if (FAILED(hr = IXMLDOMNode_get_nodeValue( impl->node, &var ))) return hr;
    if (V_VT( &var ) == VT_BSTR) hr = box_string( V_BSTR( &var ), value );
    VariantClear( &var );
    return hr;
}

static HRESULT WINAPI xml_node_put_NodeValue( IXmlNode *iface, IInspectable *value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    VARIANT var;
    HRESULT hr;

    V_VT( &var ) = VT_BSTR;
    V_BSTR( &var ) = unbox_string( value );
    hr = IXMLDOMNode_put_nodeValue( impl->node, var );
    VariantClear( &var );
    return hr;
}

static HRESULT WINAPI xml_node_get_NodeType( IXmlNode *iface, NodeType *value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    *value = (NodeType)impl->type; /* the same numbers as the DOM's */
    return S_OK;
}

static HRESULT WINAPI xml_node_get_NodeName( IXmlNode *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    BSTR name = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMNode_get_nodeName( impl->node, &name ))) return hr;
    hr = hstring_from_bstr( name, value );
    SysFreeString( name );
    return hr;
}

static HRESULT WINAPI xml_node_get_ParentNode( IXmlNode *iface, IXmlNode **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_get_parentNode( impl->node, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)value );
}

static HRESULT WINAPI xml_node_get_ChildNodes( IXmlNode *iface, IXmlNodeList **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNodeList *list = NULL;
    HRESULT hr = IXMLDOMNode_get_childNodes( impl->node, &list );
    return wrap_list_result( hr, list, value );
}

static HRESULT WINAPI xml_node_get_FirstChild( IXmlNode *iface, IXmlNode **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_get_firstChild( impl->node, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)value );
}

static HRESULT WINAPI xml_node_get_LastChild( IXmlNode *iface, IXmlNode **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_get_lastChild( impl->node, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)value );
}

static HRESULT WINAPI xml_node_get_PreviousSibling( IXmlNode *iface, IXmlNode **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_get_previousSibling( impl->node, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)value );
}

static HRESULT WINAPI xml_node_get_NextSibling( IXmlNode *iface, IXmlNode **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_get_nextSibling( impl->node, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)value );
}

static HRESULT WINAPI xml_node_get_Attributes( IXmlNode *iface, IXmlNamedNodeMap **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_node_HasChildNodes( IXmlNode *iface, boolean *value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    VARIANT_BOOL has = VARIANT_FALSE;
    HRESULT hr = IXMLDOMNode_hasChildNodes( impl->node, &has );
    *value = has == VARIANT_TRUE;
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT WINAPI xml_node_get_OwnerDocument( IXmlNode *iface, IXmlDocument **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMDocument *doc = NULL;
    HRESULT hr = IXMLDOMNode_get_ownerDocument( impl->node, &doc );
    return wrap_result( hr, (IXMLDOMNode *)doc, &IID_IXmlDocument, (void **)value );
}

static HRESULT WINAPI xml_node_InsertBefore( IXmlNode *iface, IXmlNode *new_child, IXmlNode *reference_child,
                                             IXmlNode **inserted_child )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *child = xml_node_unwrap( (IUnknown *)new_child ), *ref = xml_node_unwrap( (IUnknown *)reference_child );
    IXMLDOMNode *node = NULL;
    VARIANT var;

    if (!child) return E_INVALIDARG;
    if (ref)
    {
        V_VT( &var ) = VT_UNKNOWN;
        V_UNKNOWN( &var ) = (IUnknown *)ref;
    }
    else V_VT( &var ) = VT_NULL;
    return wrap_result( IXMLDOMNode_insertBefore( impl->node, child, var, &node ), node, &IID_IXmlNode,
                        (void **)inserted_child );
}

static HRESULT WINAPI xml_node_ReplaceChild( IXmlNode *iface, IXmlNode *new_child, IXmlNode *reference_child,
                                             IXmlNode **previous_child )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *child = xml_node_unwrap( (IUnknown *)new_child ), *ref = xml_node_unwrap( (IUnknown *)reference_child );
    IXMLDOMNode *node = NULL;

    if (!child || !ref) return E_INVALIDARG;
    return wrap_result( IXMLDOMNode_replaceChild( impl->node, child, ref, &node ), node, &IID_IXmlNode,
                        (void **)previous_child );
}

static HRESULT WINAPI xml_node_RemoveChild( IXmlNode *iface, IXmlNode *child_node, IXmlNode **removed_child )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *child = xml_node_unwrap( (IUnknown *)child_node ), *node = NULL;

    if (!child) return E_INVALIDARG;
    return wrap_result( IXMLDOMNode_removeChild( impl->node, child, &node ), node, &IID_IXmlNode,
                        (void **)removed_child );
}

static HRESULT WINAPI xml_node_AppendChild( IXmlNode *iface, IXmlNode *new_child, IXmlNode **appended_child )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *child = xml_node_unwrap( (IUnknown *)new_child ), *node = NULL;

    if (!child) return E_INVALIDARG;
    return wrap_result( IXMLDOMNode_appendChild( impl->node, child, &node ), node, &IID_IXmlNode,
                        (void **)appended_child );
}

static HRESULT WINAPI xml_node_CloneNode( IXmlNode *iface, boolean deep, IXmlNode **new_node )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMNode_cloneNode( impl->node, deep ? VARIANT_TRUE : VARIANT_FALSE, &node );
    return wrap_result( hr, node, &IID_IXmlNode, (void **)new_node );
}

static HRESULT WINAPI xml_node_get_NamespaceUri( IXmlNode *iface, IInspectable **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    BSTR str = NULL;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = IXMLDOMNode_get_namespaceURI( impl->node, &str ))) return hr;
    hr = box_string( str, value );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_node_get_LocalName( IXmlNode *iface, IInspectable **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    BSTR str = NULL;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = IXMLDOMNode_get_baseName( impl->node, &str ))) return hr;
    hr = box_string( str, value );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_node_get_Prefix( IXmlNode *iface, IInspectable **value )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    BSTR str = NULL;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = IXMLDOMNode_get_prefix( impl->node, &str ))) return hr;
    hr = box_string( str, value );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_node_Normalize( IXmlNode *iface )
{
    struct xml_node *impl = impl_from_IXmlNode( iface );
    IXMLDOMElement *element;
    HRESULT hr;

    if (FAILED(IXMLDOMNode_QueryInterface( impl->node, &IID_IXMLDOMElement, (void **)&element ))) return S_OK;
    hr = IXMLDOMElement_normalize( element );
    IXMLDOMElement_Release( element );
    return hr;
}

static HRESULT WINAPI xml_node_put_Prefix( IXmlNode *iface, IInspectable *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static const struct IXmlNodeVtbl xml_node_vtbl =
{
    xml_node_QueryInterface,
    xml_node_AddRef,
    xml_node_Release,
    /* IInspectable methods */
    xml_node_GetIids,
    xml_node_GetRuntimeClassName,
    xml_node_GetTrustLevel,
    /* IXmlNode methods */
    xml_node_get_NodeValue,
    xml_node_put_NodeValue,
    xml_node_get_NodeType,
    xml_node_get_NodeName,
    xml_node_get_ParentNode,
    xml_node_get_ChildNodes,
    xml_node_get_FirstChild,
    xml_node_get_LastChild,
    xml_node_get_PreviousSibling,
    xml_node_get_NextSibling,
    xml_node_get_Attributes,
    xml_node_HasChildNodes,
    xml_node_get_OwnerDocument,
    xml_node_InsertBefore,
    xml_node_ReplaceChild,
    xml_node_RemoveChild,
    xml_node_AppendChild,
    xml_node_CloneNode,
    xml_node_get_NamespaceUri,
    xml_node_get_LocalName,
    xml_node_get_Prefix,
    xml_node_Normalize,
    xml_node_put_Prefix,
};

DEFINE_IINSPECTABLE( xml_selector, IXmlNodeSelector, struct xml_node, IXmlNode_iface )

static HRESULT WINAPI xml_selector_SelectSingleNode( IXmlNodeSelector *iface, HSTRING xpath, IXmlNode **node )
{
    struct xml_node *impl = impl_from_IXmlNodeSelector( iface );
    BSTR path = bstr_from_hstring( xpath );
    IXMLDOMNode *found = NULL;
    HRESULT hr = IXMLDOMNode_selectSingleNode( impl->node, path, &found );
    SysFreeString( path );
    return wrap_result( hr, found, &IID_IXmlNode, (void **)node );
}

static HRESULT WINAPI xml_selector_SelectNodes( IXmlNodeSelector *iface, HSTRING xpath, IXmlNodeList **node_list )
{
    struct xml_node *impl = impl_from_IXmlNodeSelector( iface );
    BSTR path = bstr_from_hstring( xpath );
    IXMLDOMNodeList *list = NULL;
    HRESULT hr = IXMLDOMNode_selectNodes( impl->node, path, &list );
    SysFreeString( path );
    return wrap_list_result( hr, list, node_list );
}

static HRESULT WINAPI xml_selector_SelectSingleNodeNS( IXmlNodeSelector *iface, HSTRING xpath, IInspectable *namespaces,
                                                       IXmlNode **node )
{
    FIXME( "iface %p, namespaces %p: ignoring the namespaces.\n", iface, namespaces );
    return xml_selector_SelectSingleNode( iface, xpath, node );
}

static HRESULT WINAPI xml_selector_SelectNodesNS( IXmlNodeSelector *iface, HSTRING xpath, IInspectable *namespaces,
                                                  IXmlNodeList **node_list )
{
    FIXME( "iface %p, namespaces %p: ignoring the namespaces.\n", iface, namespaces );
    return xml_selector_SelectNodes( iface, xpath, node_list );
}

static const struct IXmlNodeSelectorVtbl xml_selector_vtbl =
{
    xml_selector_QueryInterface,
    xml_selector_AddRef,
    xml_selector_Release,
    /* IInspectable methods */
    xml_selector_GetIids,
    xml_selector_GetRuntimeClassName,
    xml_selector_GetTrustLevel,
    /* IXmlNodeSelector methods */
    xml_selector_SelectSingleNode,
    xml_selector_SelectNodes,
    xml_selector_SelectSingleNodeNS,
    xml_selector_SelectNodesNS,
};

DEFINE_IINSPECTABLE( xml_serializer, IXmlNodeSerializer, struct xml_node, IXmlNode_iface )

static HRESULT WINAPI xml_serializer_GetXml( IXmlNodeSerializer *iface, HSTRING *outer_xml )
{
    struct xml_node *impl = impl_from_IXmlNodeSerializer( iface );
    BSTR xml = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMNode_get_xml( impl->node, &xml ))) return hr;
    hr = hstring_from_bstr( xml, outer_xml );
    SysFreeString( xml );
    return hr;
}

static HRESULT WINAPI xml_serializer_get_InnerText( IXmlNodeSerializer *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlNodeSerializer( iface );
    BSTR text = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMNode_get_text( impl->node, &text ))) return hr;
    hr = hstring_from_bstr( text, value );
    SysFreeString( text );
    return hr;
}

static HRESULT WINAPI xml_serializer_put_InnerText( IXmlNodeSerializer *iface, HSTRING value )
{
    struct xml_node *impl = impl_from_IXmlNodeSerializer( iface );
    BSTR text = bstr_from_hstring( value );
    HRESULT hr = IXMLDOMNode_put_text( impl->node, text );
    SysFreeString( text );
    return hr;
}

static const struct IXmlNodeSerializerVtbl xml_serializer_vtbl =
{
    xml_serializer_QueryInterface,
    xml_serializer_AddRef,
    xml_serializer_Release,
    /* IInspectable methods */
    xml_serializer_GetIids,
    xml_serializer_GetRuntimeClassName,
    xml_serializer_GetTrustLevel,
    /* IXmlNodeSerializer methods */
    xml_serializer_GetXml,
    xml_serializer_get_InnerText,
    xml_serializer_put_InnerText,
};

DEFINE_IINSPECTABLE( xml_document, IXmlDocument, struct xml_node, IXmlNode_iface )

static IXMLDOMDocument3 *document_of( struct xml_node *impl )
{
    return (IXMLDOMDocument3 *)impl->node; /* made from one, see xml_document_create */
}

static HRESULT WINAPI xml_document_get_Doctype( IXmlDocument *iface, IXmlDocumentType **value )
{
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI xml_document_get_Implementation( IXmlDocument *iface, IXmlDomImplementation **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_get_DocumentElement( IXmlDocument *iface, IXmlElement **value )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    IXMLDOMElement *element = NULL;
    HRESULT hr = IXMLDOMDocument3_get_documentElement( document_of( impl ), &element );
    return wrap_result( hr, (IXMLDOMNode *)element, &IID_IXmlElement, (void **)value );
}

static HRESULT WINAPI xml_document_CreateElement( IXmlDocument *iface, HSTRING tag_name, IXmlElement **new_element )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    BSTR name = bstr_from_hstring( tag_name );
    IXMLDOMElement *element = NULL;
    HRESULT hr = IXMLDOMDocument3_createElement( document_of( impl ), name, &element );
    SysFreeString( name );
    return wrap_result( hr, (IXMLDOMNode *)element, &IID_IXmlElement, (void **)new_element );
}

static HRESULT WINAPI xml_document_CreateDocumentFragment( IXmlDocument *iface, IXmlDocumentFragment **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_CreateTextNode( IXmlDocument *iface, HSTRING data, IXmlText **new_text_node )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    BSTR text = bstr_from_hstring( data );
    IXMLDOMText *node = NULL;
    HRESULT hr = IXMLDOMDocument3_createTextNode( document_of( impl ), text, &node );
    SysFreeString( text );
    return wrap_result( hr, (IXMLDOMNode *)node, &IID_IXmlText, (void **)new_text_node );
}

static HRESULT WINAPI xml_document_CreateComment( IXmlDocument *iface, HSTRING data, IXmlComment **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_CreateProcessingInstruction( IXmlDocument *iface, HSTRING target, HSTRING data,
                                                                IXmlProcessingInstruction **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_CreateAttribute( IXmlDocument *iface, HSTRING name, IXmlAttribute **new_attribute )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    BSTR str = bstr_from_hstring( name );
    IXMLDOMAttribute *attribute = NULL;
    HRESULT hr = IXMLDOMDocument3_createAttribute( document_of( impl ), str, &attribute );
    SysFreeString( str );
    return wrap_result( hr, (IXMLDOMNode *)attribute, &IID_IXmlAttribute, (void **)new_attribute );
}

static HRESULT WINAPI xml_document_CreateEntityReference( IXmlDocument *iface, HSTRING name,
                                                          IXmlEntityReference **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_GetElementsByTagName( IXmlDocument *iface, HSTRING tag_name, IXmlNodeList **elements )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    BSTR name = bstr_from_hstring( tag_name );
    IXMLDOMNodeList *list = NULL;
    HRESULT hr = IXMLDOMDocument3_getElementsByTagName( document_of( impl ), name, &list );
    SysFreeString( name );
    return wrap_list_result( hr, list, elements );
}

static HRESULT WINAPI xml_document_CreateCDataSection( IXmlDocument *iface, HSTRING data, IXmlCDataSection **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_get_DocumentUri( IXmlDocument *iface, HSTRING *value )
{
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI xml_document_CreateAttributeNS( IXmlDocument *iface, IInspectable *namespace_uri,
                                                      HSTRING qualified_name, IXmlAttribute **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_CreateElementNS( IXmlDocument *iface, IInspectable *namespace_uri,
                                                    HSTRING qualified_name, IXmlElement **value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_document_GetElementById( IXmlDocument *iface, HSTRING element_id, IXmlElement **element )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    BSTR id = bstr_from_hstring( element_id );
    IXMLDOMNode *node = NULL;
    HRESULT hr = IXMLDOMDocument3_nodeFromID( document_of( impl ), id, &node );
    SysFreeString( id );
    return wrap_result( hr, node, &IID_IXmlElement, (void **)element );
}

static HRESULT WINAPI xml_document_ImportNode( IXmlDocument *iface, IXmlNode *node, boolean deep, IXmlNode **new_node )
{
    struct xml_node *impl = impl_from_IXmlDocument( iface );
    IXMLDOMNode *source = xml_node_unwrap( (IUnknown *)node ), *copy = NULL;

    if (!source) return E_INVALIDARG;
    return wrap_result( IXMLDOMDocument3_importNode( document_of( impl ), source, deep ? VARIANT_TRUE : VARIANT_FALSE,
                                                     &copy ), copy, &IID_IXmlNode, (void **)new_node );
}

static const struct IXmlDocumentVtbl xml_document_vtbl =
{
    xml_document_QueryInterface,
    xml_document_AddRef,
    xml_document_Release,
    /* IInspectable methods */
    xml_document_GetIids,
    xml_document_GetRuntimeClassName,
    xml_document_GetTrustLevel,
    /* IXmlDocument methods */
    xml_document_get_Doctype,
    xml_document_get_Implementation,
    xml_document_get_DocumentElement,
    xml_document_CreateElement,
    xml_document_CreateDocumentFragment,
    xml_document_CreateTextNode,
    xml_document_CreateComment,
    xml_document_CreateProcessingInstruction,
    xml_document_CreateAttribute,
    xml_document_CreateEntityReference,
    xml_document_GetElementsByTagName,
    xml_document_CreateCDataSection,
    xml_document_get_DocumentUri,
    xml_document_CreateAttributeNS,
    xml_document_CreateElementNS,
    xml_document_GetElementById,
    xml_document_ImportNode,
};

DEFINE_IINSPECTABLE( xml_document_io, IXmlDocumentIO, struct xml_node, IXmlNode_iface )

static HRESULT WINAPI xml_document_io_LoadXml( IXmlDocumentIO *iface, HSTRING xml )
{
    struct xml_node *impl = impl_from_IXmlDocumentIO( iface );
    BSTR str = bstr_from_hstring( xml );
    VARIANT_BOOL ok = VARIANT_FALSE;
    HRESULT hr = IXMLDOMDocument3_loadXML( document_of( impl ), str, &ok );

    SysFreeString( str );
    if (FAILED(hr)) return hr;
    if (ok != VARIANT_TRUE)
    {
        WARN( "not well-formed: %s\n", debugstr_hstring( xml ) );
        return HRESULT_FROM_WIN32( ERROR_XML_PARSE_ERROR );
    }
    return S_OK;
}

static HRESULT WINAPI xml_document_io_LoadXmlWithSettings( IXmlDocumentIO *iface, HSTRING xml,
                                                           IXmlLoadSettings *load_settings )
{
    return xml_document_io_LoadXml( iface, xml );
}

static HRESULT WINAPI xml_document_io_SaveToFileAsync( IXmlDocumentIO *iface, IStorageFile *file,
                                                       IAsyncAction **async_info )
{
    FIXME( "iface %p, file %p stub!\n", iface, file );
    *async_info = NULL;
    return E_NOTIMPL;
}

static const struct IXmlDocumentIOVtbl xml_document_io_vtbl =
{
    xml_document_io_QueryInterface,
    xml_document_io_AddRef,
    xml_document_io_Release,
    /* IInspectable methods */
    xml_document_io_GetIids,
    xml_document_io_GetRuntimeClassName,
    xml_document_io_GetTrustLevel,
    /* IXmlDocumentIO methods */
    xml_document_io_LoadXml,
    xml_document_io_LoadXmlWithSettings,
    xml_document_io_SaveToFileAsync,
};

DEFINE_IINSPECTABLE( xml_element, IXmlElement, struct xml_node, IXmlNode_iface )

static IXMLDOMElement *element_of( struct xml_node *impl )
{
    return (IXMLDOMElement *)impl->node; /* made from one, see wrap_node */
}

static HRESULT WINAPI xml_element_get_TagName( IXmlElement *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMElement_get_tagName( element_of( impl ), &name ))) return hr;
    hr = hstring_from_bstr( name, value );
    SysFreeString( name );
    return hr;
}

/* an attribute that is not there is the empty string, as in Windows */
static HRESULT WINAPI xml_element_GetAttribute( IXmlElement *iface, HSTRING attribute_name, HSTRING *attribute_value )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = bstr_from_hstring( attribute_name );
    VARIANT var;
    HRESULT hr;

    VariantInit( &var );
    hr = IXMLDOMElement_getAttribute( element_of( impl ), name, &var );
    SysFreeString( name );
    *attribute_value = NULL;
    if (FAILED(hr)) return hr;
    if (V_VT( &var ) == VT_BSTR) hr = hstring_from_bstr( V_BSTR( &var ), attribute_value );
    else hr = S_OK;
    VariantClear( &var );
    return hr;
}

static HRESULT WINAPI xml_element_SetAttribute( IXmlElement *iface, HSTRING attribute_name, HSTRING attribute_value )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = bstr_from_hstring( attribute_name );
    VARIANT var;
    HRESULT hr;

    V_VT( &var ) = VT_BSTR;
    V_BSTR( &var ) = bstr_from_hstring( attribute_value );
    hr = IXMLDOMElement_setAttribute( element_of( impl ), name, var );
    VariantClear( &var );
    SysFreeString( name );
    return hr;
}

static HRESULT WINAPI xml_element_RemoveAttribute( IXmlElement *iface, HSTRING attribute_name )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = bstr_from_hstring( attribute_name );
    HRESULT hr = IXMLDOMElement_removeAttribute( element_of( impl ), name );
    SysFreeString( name );
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT WINAPI xml_element_GetAttributeNode( IXmlElement *iface, HSTRING attribute_name,
                                                    IXmlAttribute **attribute_node )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = bstr_from_hstring( attribute_name );
    IXMLDOMAttribute *attribute = NULL;
    HRESULT hr = IXMLDOMElement_getAttributeNode( element_of( impl ), name, &attribute );
    SysFreeString( name );
    return wrap_result( hr, (IXMLDOMNode *)attribute, &IID_IXmlAttribute, (void **)attribute_node );
}

static HRESULT WINAPI xml_element_SetAttributeNode( IXmlElement *iface, IXmlAttribute *new_attribute,
                                                    IXmlAttribute **previous_attribute )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    IXMLDOMNode *node = xml_node_unwrap( (IUnknown *)new_attribute );
    IXMLDOMAttribute *attribute, *previous = NULL;
    HRESULT hr;

    *previous_attribute = NULL;
    if (!node || FAILED(IXMLDOMNode_QueryInterface( node, &IID_IXMLDOMAttribute, (void **)&attribute )))
        return E_INVALIDARG;
    hr = IXMLDOMElement_setAttributeNode( element_of( impl ), attribute, &previous );
    IXMLDOMAttribute_Release( attribute );
    return wrap_result( hr, (IXMLDOMNode *)previous, &IID_IXmlAttribute, (void **)previous_attribute );
}

static HRESULT WINAPI xml_element_RemoveAttributeNode( IXmlElement *iface, IXmlAttribute *attribute_node,
                                                       IXmlAttribute **removed_attribute )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    IXMLDOMNode *node = xml_node_unwrap( (IUnknown *)attribute_node );
    IXMLDOMAttribute *attribute, *removed = NULL;
    HRESULT hr;

    *removed_attribute = NULL;
    if (!node || FAILED(IXMLDOMNode_QueryInterface( node, &IID_IXMLDOMAttribute, (void **)&attribute )))
        return E_INVALIDARG;
    hr = IXMLDOMElement_removeAttributeNode( element_of( impl ), attribute, &removed );
    IXMLDOMAttribute_Release( attribute );
    return wrap_result( hr, (IXMLDOMNode *)removed, &IID_IXmlAttribute, (void **)removed_attribute );
}

static HRESULT WINAPI xml_element_GetElementsByTagName( IXmlElement *iface, HSTRING tag_name, IXmlNodeList **elements )
{
    struct xml_node *impl = impl_from_IXmlElement( iface );
    BSTR name = bstr_from_hstring( tag_name );
    IXMLDOMNodeList *list = NULL;
    HRESULT hr = IXMLDOMElement_getElementsByTagName( element_of( impl ), name, &list );
    SysFreeString( name );
    return wrap_list_result( hr, list, elements );
}

static HRESULT WINAPI xml_element_SetAttributeNS( IXmlElement *iface, IInspectable *namespace_uri,
                                                  HSTRING qualified_name, HSTRING value )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_element_GetAttributeNS( IXmlElement *iface, IInspectable *namespace_uri,
                                                  HSTRING local_name, HSTRING *value )
{
    FIXME( "iface %p stub!\n", iface );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_element_RemoveAttributeNS( IXmlElement *iface, IInspectable *namespace_uri,
                                                     HSTRING local_name )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_element_SetAttributeNodeNS( IXmlElement *iface, IXmlAttribute *new_attribute,
                                                      IXmlAttribute **previous_attribute )
{
    FIXME( "iface %p stub!\n", iface );
    *previous_attribute = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_element_GetAttributeNodeNS( IXmlElement *iface, IInspectable *namespace_uri,
                                                      HSTRING local_name, IXmlAttribute **previous_attribute )
{
    FIXME( "iface %p stub!\n", iface );
    *previous_attribute = NULL;
    return E_NOTIMPL;
}

static const struct IXmlElementVtbl xml_element_vtbl =
{
    xml_element_QueryInterface,
    xml_element_AddRef,
    xml_element_Release,
    /* IInspectable methods */
    xml_element_GetIids,
    xml_element_GetRuntimeClassName,
    xml_element_GetTrustLevel,
    /* IXmlElement methods */
    xml_element_get_TagName,
    xml_element_GetAttribute,
    xml_element_SetAttribute,
    xml_element_RemoveAttribute,
    xml_element_GetAttributeNode,
    xml_element_SetAttributeNode,
    xml_element_RemoveAttributeNode,
    xml_element_GetElementsByTagName,
    xml_element_SetAttributeNS,
    xml_element_GetAttributeNS,
    xml_element_RemoveAttributeNS,
    xml_element_SetAttributeNodeNS,
    xml_element_GetAttributeNodeNS,
};

DEFINE_IINSPECTABLE( xml_chardata, IXmlCharacterData, struct xml_node, IXmlNode_iface )

static IXMLDOMCharacterData *chardata_of( struct xml_node *impl )
{
    return (IXMLDOMCharacterData *)impl->node; /* made from one, see wrap_node */
}

static HRESULT WINAPI xml_chardata_get_Data( IXmlCharacterData *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR data = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMCharacterData_get_data( chardata_of( impl ), &data ))) return hr;
    hr = hstring_from_bstr( data, value );
    SysFreeString( data );
    return hr;
}

static HRESULT WINAPI xml_chardata_put_Data( IXmlCharacterData *iface, HSTRING value )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR data = bstr_from_hstring( value );
    HRESULT hr = IXMLDOMCharacterData_put_data( chardata_of( impl ), data );
    SysFreeString( data );
    return hr;
}

static HRESULT WINAPI xml_chardata_get_Length( IXmlCharacterData *iface, UINT32 *value )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    LONG length = 0;
    HRESULT hr = IXMLDOMCharacterData_get_length( chardata_of( impl ), &length );
    *value = length;
    return hr;
}

static HRESULT WINAPI xml_chardata_SubstringData( IXmlCharacterData *iface, UINT32 offset, UINT32 count, HSTRING *data )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR str = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMCharacterData_substringData( chardata_of( impl ), offset, count, &str ))) return hr;
    hr = hstring_from_bstr( str, data );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_chardata_AppendData( IXmlCharacterData *iface, HSTRING data )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR str = bstr_from_hstring( data );
    HRESULT hr = IXMLDOMCharacterData_appendData( chardata_of( impl ), str );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_chardata_InsertData( IXmlCharacterData *iface, UINT32 offset, HSTRING data )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR str = bstr_from_hstring( data );
    HRESULT hr = IXMLDOMCharacterData_insertData( chardata_of( impl ), offset, str );
    SysFreeString( str );
    return hr;
}

static HRESULT WINAPI xml_chardata_DeleteData( IXmlCharacterData *iface, UINT32 offset, UINT32 count )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    return IXMLDOMCharacterData_deleteData( chardata_of( impl ), offset, count );
}

static HRESULT WINAPI xml_chardata_ReplaceData( IXmlCharacterData *iface, UINT32 offset, UINT32 count, HSTRING data )
{
    struct xml_node *impl = impl_from_IXmlCharacterData( iface );
    BSTR str = bstr_from_hstring( data );
    HRESULT hr = IXMLDOMCharacterData_replaceData( chardata_of( impl ), offset, count, str );
    SysFreeString( str );
    return hr;
}

static const struct IXmlCharacterDataVtbl xml_chardata_vtbl =
{
    xml_chardata_QueryInterface,
    xml_chardata_AddRef,
    xml_chardata_Release,
    /* IInspectable methods */
    xml_chardata_GetIids,
    xml_chardata_GetRuntimeClassName,
    xml_chardata_GetTrustLevel,
    /* IXmlCharacterData methods */
    xml_chardata_get_Data,
    xml_chardata_put_Data,
    xml_chardata_get_Length,
    xml_chardata_SubstringData,
    xml_chardata_AppendData,
    xml_chardata_InsertData,
    xml_chardata_DeleteData,
    xml_chardata_ReplaceData,
};

DEFINE_IINSPECTABLE( xml_text, IXmlText, struct xml_node, IXmlNode_iface )

static HRESULT WINAPI xml_text_SplitText( IXmlText *iface, UINT32 offset, IXmlText **second_part )
{
    struct xml_node *impl = impl_from_IXmlText( iface );
    IXMLDOMText *text, *second = NULL;
    HRESULT hr;

    *second_part = NULL;
    if (FAILED(hr = IXMLDOMNode_QueryInterface( impl->node, &IID_IXMLDOMText, (void **)&text ))) return hr;
    hr = IXMLDOMText_splitText( text, offset, &second );
    IXMLDOMText_Release( text );
    return wrap_result( hr, (IXMLDOMNode *)second, &IID_IXmlText, (void **)second_part );
}

static const struct IXmlTextVtbl xml_text_vtbl =
{
    xml_text_QueryInterface,
    xml_text_AddRef,
    xml_text_Release,
    /* IInspectable methods */
    xml_text_GetIids,
    xml_text_GetRuntimeClassName,
    xml_text_GetTrustLevel,
    /* IXmlText methods */
    xml_text_SplitText,
};

DEFINE_IINSPECTABLE( xml_attribute, IXmlAttribute, struct xml_node, IXmlNode_iface )

static IXMLDOMAttribute *attribute_of( struct xml_node *impl )
{
    return (IXMLDOMAttribute *)impl->node; /* made from one, see wrap_node */
}

static HRESULT WINAPI xml_attribute_get_Name( IXmlAttribute *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlAttribute( iface );
    BSTR name = NULL;
    HRESULT hr;

    if (FAILED(hr = IXMLDOMAttribute_get_name( attribute_of( impl ), &name ))) return hr;
    hr = hstring_from_bstr( name, value );
    SysFreeString( name );
    return hr;
}

static HRESULT WINAPI xml_attribute_get_Specified( IXmlAttribute *iface, boolean *value )
{
    struct xml_node *impl = impl_from_IXmlAttribute( iface );
    VARIANT_BOOL specified = VARIANT_TRUE;
    HRESULT hr = IXMLDOMNode_get_specified( impl->node, &specified );
    *value = specified == VARIANT_TRUE;
    return FAILED(hr) ? hr : S_OK;
}

static HRESULT WINAPI xml_attribute_get_Value( IXmlAttribute *iface, HSTRING *value )
{
    struct xml_node *impl = impl_from_IXmlAttribute( iface );
    VARIANT var;
    HRESULT hr;

    VariantInit( &var );
    *value = NULL;
    if (FAILED(hr = IXMLDOMAttribute_get_value( attribute_of( impl ), &var ))) return hr;
    if (V_VT( &var ) == VT_BSTR) hr = hstring_from_bstr( V_BSTR( &var ), value );
    VariantClear( &var );
    return hr;
}

static HRESULT WINAPI xml_attribute_put_Value( IXmlAttribute *iface, HSTRING value )
{
    struct xml_node *impl = impl_from_IXmlAttribute( iface );
    VARIANT var;
    HRESULT hr;

    V_VT( &var ) = VT_BSTR;
    V_BSTR( &var ) = bstr_from_hstring( value );
    hr = IXMLDOMAttribute_put_value( attribute_of( impl ), var );
    VariantClear( &var );
    return hr;
}

static const struct IXmlAttributeVtbl xml_attribute_vtbl =
{
    xml_attribute_QueryInterface,
    xml_attribute_AddRef,
    xml_attribute_Release,
    /* IInspectable methods */
    xml_attribute_GetIids,
    xml_attribute_GetRuntimeClassName,
    xml_attribute_GetTrustLevel,
    /* IXmlAttribute methods */
    xml_attribute_get_Name,
    xml_attribute_get_Specified,
    xml_attribute_get_Value,
    xml_attribute_put_Value,
};

/* the object keeps msxml's node as the interface of its type, which the
 * type-specific methods above call through */
static HRESULT wrap_node( IXMLDOMNode *node, REFIID iid, void **out )
{
    struct xml_node *impl;
    DOMNodeType type;
    const IID *typed = &IID_IXMLDOMNode;
    IXMLDOMNode *held;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = IXMLDOMNode_get_nodeType( node, &type ))) return hr;
    switch (type)
    {
    case NODE_DOCUMENT:      typed = &IID_IXMLDOMDocument3; break;
    case NODE_ELEMENT:       typed = &IID_IXMLDOMElement; break;
    case NODE_TEXT:
    case NODE_CDATA_SECTION:
    case NODE_COMMENT:       typed = &IID_IXMLDOMCharacterData; break;
    case NODE_ATTRIBUTE:     typed = &IID_IXMLDOMAttribute; break;
    default: break;
    }
    if (FAILED(IXMLDOMNode_QueryInterface( node, typed, (void **)&held )))
    {
        /* only a node, then: none of the interfaces of its type */
        WARN( "node type %d without %s\n", type, debugstr_guid( typed ) );
        IXMLDOMNode_AddRef( held = node );
        type = NODE_INVALID;
    }
    if (!(impl = calloc( 1, sizeof(*impl) )))
    {
        IXMLDOMNode_Release( held );
        return E_OUTOFMEMORY;
    }
    impl->IXmlNode_iface.lpVtbl = &xml_node_vtbl;
    impl->IXmlNodeSelector_iface.lpVtbl = &xml_selector_vtbl;
    impl->IXmlNodeSerializer_iface.lpVtbl = &xml_serializer_vtbl;
    impl->IXmlDocument_iface.lpVtbl = &xml_document_vtbl;
    impl->IXmlDocumentIO_iface.lpVtbl = &xml_document_io_vtbl;
    impl->IXmlElement_iface.lpVtbl = &xml_element_vtbl;
    impl->IXmlCharacterData_iface.lpVtbl = &xml_chardata_vtbl;
    impl->IXmlText_iface.lpVtbl = &xml_text_vtbl;
    impl->IXmlAttribute_iface.lpVtbl = &xml_attribute_vtbl;
    impl->ref = 1;
    impl->node = held;
    impl->type = type;

    hr = IXmlNode_QueryInterface( &impl->IXmlNode_iface, iid, out );
    IXmlNode_Release( &impl->IXmlNode_iface );
    return hr;
}

HRESULT xml_document_create( IXMLDOMDocument3 *doc, IXmlDocument **out )
{
    return wrap_node( (IXMLDOMNode *)doc, &IID_IXmlDocument, (void **)out );
}

static HRESULT new_msxml_document( IXMLDOMDocument3 **doc )
{
    HRESULT hr = CoCreateInstance( &CLSID_DOMDocument60, NULL, CLSCTX_INPROC_SERVER, &IID_IXMLDOMDocument3,
                                   (void **)doc );
    if (FAILED(hr)) ERR( "no msxml6 document: %#lx\n", hr );
    return hr;
}

HRESULT xml_document_load( const WCHAR *xml, IXmlDocument **out )
{
    IXMLDOMDocument3 *doc;
    VARIANT_BOOL ok = VARIANT_FALSE;
    BSTR str;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = new_msxml_document( &doc ))) return hr;
    str = SysAllocString( xml );
    hr = IXMLDOMDocument3_loadXML( doc, str, &ok );
    SysFreeString( str );
    if (SUCCEEDED(hr) && ok == VARIANT_TRUE) hr = xml_document_create( doc, out );
    else if (SUCCEEDED(hr)) hr = HRESULT_FROM_WIN32( ERROR_XML_PARSE_ERROR );
    IXMLDOMDocument3_Release( doc );
    return hr;
}

struct xml_node_list
{
    IXmlNodeList IXmlNodeList_iface;
    IVectorView_IXmlNode IVectorView_IXmlNode_iface;
    IIterable_IXmlNode IIterable_IXmlNode_iface;
    LONG ref;

    IXMLDOMNodeList *list;
};

static inline struct xml_node_list *impl_from_IXmlNodeList( IXmlNodeList *iface )
{
    return CONTAINING_RECORD( iface, struct xml_node_list, IXmlNodeList_iface );
}

static HRESULT WINAPI xml_list_QueryInterface( IXmlNodeList *iface, REFIID iid, void **out )
{
    struct xml_node_list *impl = impl_from_IXmlNodeList( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IXmlNodeList ))
        *out = &impl->IXmlNodeList_iface;
    else if (IsEqualGUID( iid, &IID_IVectorView_IXmlNode ))
        *out = &impl->IVectorView_IXmlNode_iface;
    else if (IsEqualGUID( iid, &IID_IIterable_IXmlNode ))
        *out = &impl->IIterable_IXmlNode_iface;

    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI xml_list_AddRef( IXmlNodeList *iface )
{
    struct xml_node_list *impl = impl_from_IXmlNodeList( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI xml_list_Release( IXmlNodeList *iface )
{
    struct xml_node_list *impl = impl_from_IXmlNodeList( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IXMLDOMNodeList_Release( impl->list );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI xml_list_GetIids( IXmlNodeList *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_list_GetRuntimeClassName( IXmlNodeList *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Data_Xml_Dom_XmlNodeList,
                                wcslen( RuntimeClass_Windows_Data_Xml_Dom_XmlNodeList ), class_name );
}

static HRESULT WINAPI xml_list_GetTrustLevel( IXmlNodeList *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI xml_list_get_Length( IXmlNodeList *iface, UINT32 *value )
{
    struct xml_node_list *impl = impl_from_IXmlNodeList( iface );
    LONG length = 0;
    HRESULT hr = IXMLDOMNodeList_get_length( impl->list, &length );
    *value = length;
    return hr;
}

static HRESULT WINAPI xml_list_Item( IXmlNodeList *iface, UINT32 index, IXmlNode **node )
{
    struct xml_node_list *impl = impl_from_IXmlNodeList( iface );
    IXMLDOMNode *item = NULL;
    HRESULT hr = IXMLDOMNodeList_get_item( impl->list, index, &item );
    return wrap_result( hr, item, &IID_IXmlNode, (void **)node );
}

static const struct IXmlNodeListVtbl xml_list_vtbl =
{
    xml_list_QueryInterface,
    xml_list_AddRef,
    xml_list_Release,
    /* IInspectable methods */
    xml_list_GetIids,
    xml_list_GetRuntimeClassName,
    xml_list_GetTrustLevel,
    /* IXmlNodeList methods */
    xml_list_get_Length,
    xml_list_Item,
};

DEFINE_IINSPECTABLE( xml_vector, IVectorView_IXmlNode, struct xml_node_list, IXmlNodeList_iface )

static HRESULT WINAPI xml_vector_GetAt( IVectorView_IXmlNode *iface, UINT32 index, IXmlNode **value )
{
    struct xml_node_list *impl = impl_from_IVectorView_IXmlNode( iface );
    UINT32 size;

    *value = NULL;
    xml_list_get_Length( &impl->IXmlNodeList_iface, &size );
    if (index >= size) return E_BOUNDS;
    return xml_list_Item( &impl->IXmlNodeList_iface, index, value );
}

static HRESULT WINAPI xml_vector_get_Size( IVectorView_IXmlNode *iface, UINT32 *value )
{
    struct xml_node_list *impl = impl_from_IVectorView_IXmlNode( iface );
    return xml_list_get_Length( &impl->IXmlNodeList_iface, value );
}

static HRESULT WINAPI xml_vector_IndexOf( IVectorView_IXmlNode *iface, IXmlNode *element, UINT32 *index, BOOLEAN *found )
{
    struct xml_node_list *impl = impl_from_IVectorView_IXmlNode( iface );
    IXMLDOMNode *target = xml_node_unwrap( (IUnknown *)element ), *item;
    LONG length = 0, i;

    *index = 0;
    *found = FALSE;
    IXMLDOMNodeList_get_length( impl->list, &length );
    for (i = 0; target && i < length; i++)
    {
        if (FAILED(IXMLDOMNodeList_get_item( impl->list, i, &item )) || !item) continue;
        if (item == target) *found = TRUE;
        IXMLDOMNode_Release( item );
        if (*found)
        {
            *index = i;
            break;
        }
    }
    return S_OK;
}

static HRESULT WINAPI xml_vector_GetMany( IVectorView_IXmlNode *iface, UINT32 start_index, UINT32 items_size,
                                          IXmlNode **items, UINT32 *value )
{
    struct xml_node_list *impl = impl_from_IVectorView_IXmlNode( iface );
    UINT32 size, i;
    HRESULT hr;

    *value = 0;
    xml_list_get_Length( &impl->IXmlNodeList_iface, &size );
    if (start_index > size) return E_BOUNDS;
    for (i = 0; start_index + i < size && i < items_size; i++)
    {
        if (FAILED(hr = xml_list_Item( &impl->IXmlNodeList_iface, start_index + i, &items[i] )))
        {
            while (i--) IXmlNode_Release( items[i] );
            return hr;
        }
    }
    *value = i;
    return S_OK;
}

static const struct IVectorView_IXmlNodeVtbl xml_vector_vtbl =
{
    xml_vector_QueryInterface,
    xml_vector_AddRef,
    xml_vector_Release,
    /* IInspectable methods */
    xml_vector_GetIids,
    xml_vector_GetRuntimeClassName,
    xml_vector_GetTrustLevel,
    /* IVectorView<IXmlNode> methods */
    xml_vector_GetAt,
    xml_vector_get_Size,
    xml_vector_IndexOf,
    xml_vector_GetMany,
};

struct xml_iterator
{
    IIterator_IXmlNode IIterator_IXmlNode_iface;
    LONG ref;

    struct xml_node_list *list;
    UINT32 index;
};

static inline struct xml_iterator *impl_from_IIterator_IXmlNode( IIterator_IXmlNode *iface )
{
    return CONTAINING_RECORD( iface, struct xml_iterator, IIterator_IXmlNode_iface );
}

static HRESULT WINAPI xml_iterator_QueryInterface( IIterator_IXmlNode *iface, REFIID iid, void **out )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterator_IXmlNode ))
    {
        *out = &impl->IIterator_IXmlNode_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI xml_iterator_AddRef( IIterator_IXmlNode *iface )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI xml_iterator_Release( IIterator_IXmlNode *iface )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        IXmlNodeList_Release( &impl->list->IXmlNodeList_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI xml_iterator_GetIids( IIterator_IXmlNode *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_iterator_GetRuntimeClassName( IIterator_IXmlNode *iface, HSTRING *class_name )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI xml_iterator_GetTrustLevel( IIterator_IXmlNode *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI xml_iterator_get_Current( IIterator_IXmlNode *iface, IXmlNode **value )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    return xml_vector_GetAt( &impl->list->IVectorView_IXmlNode_iface, impl->index, value );
}

static HRESULT WINAPI xml_iterator_get_HasCurrent( IIterator_IXmlNode *iface, boolean *value )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    UINT32 size;

    xml_list_get_Length( &impl->list->IXmlNodeList_iface, &size );
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI xml_iterator_MoveNext( IIterator_IXmlNode *iface, boolean *value )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    UINT32 size;

    xml_list_get_Length( &impl->list->IXmlNodeList_iface, &size );
    if (impl->index < size) impl->index++;
    *value = impl->index < size;
    return S_OK;
}

static HRESULT WINAPI xml_iterator_GetMany( IIterator_IXmlNode *iface, UINT32 items_size, IXmlNode **items,
                                            UINT32 *value )
{
    struct xml_iterator *impl = impl_from_IIterator_IXmlNode( iface );
    HRESULT hr = xml_vector_GetMany( &impl->list->IVectorView_IXmlNode_iface, impl->index, items_size, items, value );
    if (SUCCEEDED(hr)) impl->index += *value;
    return hr;
}

static const struct IIterator_IXmlNodeVtbl xml_iterator_vtbl =
{
    xml_iterator_QueryInterface,
    xml_iterator_AddRef,
    xml_iterator_Release,
    /* IInspectable methods */
    xml_iterator_GetIids,
    xml_iterator_GetRuntimeClassName,
    xml_iterator_GetTrustLevel,
    /* IIterator<IXmlNode> methods */
    xml_iterator_get_Current,
    xml_iterator_get_HasCurrent,
    xml_iterator_MoveNext,
    xml_iterator_GetMany,
};

DEFINE_IINSPECTABLE( xml_iterable, IIterable_IXmlNode, struct xml_node_list, IXmlNodeList_iface )

static HRESULT WINAPI xml_iterable_First( IIterable_IXmlNode *iface, IIterator_IXmlNode **value )
{
    struct xml_node_list *impl = impl_from_IIterable_IXmlNode( iface );
    struct xml_iterator *iter;

    *value = NULL;
    if (!(iter = calloc( 1, sizeof(*iter) ))) return E_OUTOFMEMORY;
    iter->IIterator_IXmlNode_iface.lpVtbl = &xml_iterator_vtbl;
    iter->ref = 1;
    iter->list = impl;
    IXmlNodeList_AddRef( &impl->IXmlNodeList_iface );
    *value = &iter->IIterator_IXmlNode_iface;
    return S_OK;
}

static const struct IIterable_IXmlNodeVtbl xml_iterable_vtbl =
{
    xml_iterable_QueryInterface,
    xml_iterable_AddRef,
    xml_iterable_Release,
    /* IInspectable methods */
    xml_iterable_GetIids,
    xml_iterable_GetRuntimeClassName,
    xml_iterable_GetTrustLevel,
    /* IIterable<IXmlNode> methods */
    xml_iterable_First,
};

static HRESULT wrap_list( IXMLDOMNodeList *list, IXmlNodeList **out )
{
    struct xml_node_list *impl;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IXmlNodeList_iface.lpVtbl = &xml_list_vtbl;
    impl->IVectorView_IXmlNode_iface.lpVtbl = &xml_vector_vtbl;
    impl->IIterable_IXmlNode_iface.lpVtbl = &xml_iterable_vtbl;
    impl->ref = 1;
    impl->list = list;
    IXMLDOMNodeList_AddRef( list );
    *out = &impl->IXmlNodeList_iface;
    return S_OK;
}

/* the class: new XmlDocument() is an empty document */
struct xml_document_statics
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct xml_document_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct xml_document_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct xml_document_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct xml_document_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct xml_document_statics *impl = impl_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    IXMLDOMDocument3 *doc;
    HRESULT hr;

    TRACE( "iface %p, instance %p.\n", iface, instance );

    *instance = NULL;
    if (FAILED(hr = new_msxml_document( &doc ))) return hr;
    hr = xml_document_create( doc, (IXmlDocument **)instance );
    IXMLDOMDocument3_Release( doc );
    return hr;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static struct xml_document_statics xml_document_statics =
{
    {&factory_vtbl},
    1,
};

IActivationFactory *xml_document_factory = &xml_document_statics.IActivationFactory_iface;
