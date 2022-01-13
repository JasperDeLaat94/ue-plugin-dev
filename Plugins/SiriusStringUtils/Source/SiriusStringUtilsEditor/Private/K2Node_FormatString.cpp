// Copyright 2022-2022 Jasper de Laat. All Rights Reserved.

#include "K2Node_FormatString.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "EditorCategoryUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "KismetCompiler.h"
#include "SiriusStringLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetTextLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"

#define LOCTEXT_NAMESPACE "K2Node_FormatString"

UK2Node_FormatString::UK2Node_FormatString(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	  , CachedFormatPin(nullptr)
{
	// TODO (Jasper): Argument types still restricted?
	NodeTooltip = LOCTEXT("NodeTooltip", "Builds a formatted string using available format argument values.\n  \u2022 Use {} to denote format arguments.\n  \u2022 Argument types may be Byte, Int, Int64, Float, Text, String, Name, Boolean or Object.");
}

void UK2Node_FormatString::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UK2Node_FormatString, PinNames))
	{
		ReconstructNode();
		GetGraph()->NotifyGraphChanged();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UK2Node_FormatString::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();

	CachedFormatPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, TEXT("Format"));
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, TEXT("Result"));

	for (const FName& PinName : PinNames)
	{
		CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, PinName);
	}
}

FText UK2Node_FormatString::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("NodeTitle", "Format String");
}

void UK2Node_FormatString::PinConnectionListChanged(UEdGraphPin* Pin)
{
	UEdGraphPin* FormatPin = GetFormatPin();

	Modify();

	// Clear all pins.
	if (Pin == FormatPin && !FormatPin->DefaultTextValue.IsEmpty())
	{
		PinNames.Empty();
		GetSchema()->TrySetDefaultText(*FormatPin, FText::GetEmpty());

		for (auto It = Pins.CreateConstIterator(); It; ++It)
		{
			UEdGraphPin* CheckPin = *It;
			if (CheckPin != FormatPin && CheckPin->Direction == EGPD_Input)
			{
				CheckPin->Modify();
				CheckPin->MarkPendingKill();
				Pins.Remove(CheckPin);
				--It;
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
	}

	// Potentially update an argument pin type
	SynchronizeArgumentPinType(Pin);
}

void UK2Node_FormatString::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	const UEdGraphPin* FormatPin = GetFormatPin();
	if (Pin == FormatPin && FormatPin->LinkedTo.Num() == 0)
	{
		TArray<FString> ArgumentParams;
		FTextFormat::FromString(FormatPin->DefaultValue).GetFormatArgumentNames(ArgumentParams);

		PinNames.Reset();

		for (const FString& Param : ArgumentParams)
		{
			const FName ParamName(*Param);
			if (!FindArgumentPin(ParamName))
			{
				CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, ParamName);
			}
			PinNames.Add(ParamName);
		}

		for (auto It = Pins.CreateIterator(); It; ++It)
		{
			UEdGraphPin* CheckPin = *It;
			if (CheckPin != FormatPin && CheckPin->Direction == EGPD_Input)
			{
				const bool bIsValidArgPin = ArgumentParams.ContainsByPredicate([&CheckPin](const FString& InPinName)
				{
					return InPinName.Equals(CheckPin->PinName.ToString(), ESearchCase::CaseSensitive);
				});

				if (!bIsValidArgPin)
				{
					CheckPin->MarkPendingKill();
					It.RemoveCurrent();
				}
			}
		}

		GetGraph()->NotifyGraphChanged();
	}
}

void UK2Node_FormatString::PinTypeChanged(UEdGraphPin* Pin)
{
	// Potentially update an argument pin type
	SynchronizeArgumentPinType(Pin);

	Super::PinTypeChanged(Pin);
}

FText UK2Node_FormatString::GetTooltipText() const
{
	return NodeTooltip;
}

FText UK2Node_FormatString::GetPinDisplayName(const UEdGraphPin* Pin) const
{
	return FText::FromName(Pin->PinName);
}

void UK2Node_FormatString::PostReconstructNode()
{
	Super::PostReconstructNode();

	if (!IsTemplate())
	{
		// Make sure we're not dealing with a menu node
		UEdGraph* OuterGraph = GetGraph();
		if (OuterGraph && OuterGraph->Schema)
		{
			for (UEdGraphPin* CurrentPin : Pins)
			{
				// Potentially update an argument pin type
				SynchronizeArgumentPinType(CurrentPin);
			}
		}
	}
}

void UK2Node_FormatString::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	/**
		At the end of this, the UK2Node_FormatString will not be a part of the Blueprint, it merely handles connecting
		the other nodes into the Blueprint.
	*/

	// Create a "Make Array" node to compile the list of arguments into an array for the Format function being called
	UK2Node_MakeArray* MakeArrayNode = CompilerContext.SpawnIntermediateNode<UK2Node_MakeArray>(this, SourceGraph);
	MakeArrayNode->AllocateDefaultPins();
	CompilerContext.MessageLog.NotifyIntermediateObjectCreation(MakeArrayNode, this);

	UEdGraphPin* ArrayOut = MakeArrayNode->GetOutputPin();

	// This is the node that does all the Format work.
	UK2Node_CallFunction* CallFormatFunction = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallFormatFunction->SetFromFunction(USiriusStringLibrary::StaticClass()->FindFunctionByName(GET_MEMBER_NAME_CHECKED(USiriusStringLibrary, Format)));
	CallFormatFunction->AllocateDefaultPins();
	CompilerContext.MessageLog.NotifyIntermediateObjectCreation(CallFormatFunction, this);

	// Connect the output of the "Make Array" pin to the function's "InArgs" pin
	ArrayOut->MakeLinkTo(CallFormatFunction->FindPinChecked(TEXT("InArgs")));

	// This will set the "Make Array" node's type, only works if one pin is connected.
	MakeArrayNode->PinConnectionListChanged(ArrayOut);

	// For each argument, we will need to add in a "Make Struct" node.
	for(int32 ArgIdx = 0; ArgIdx < PinNames.Num(); ++ArgIdx)
	{
		UEdGraphPin* ArgumentPin = FindArgumentPin(PinNames[ArgIdx]);

		static UScriptStruct* FormatArgumentDataStruct = FindObjectChecked<UScriptStruct>(FindObjectChecked<UPackage>(nullptr, TEXT("/Script/SiriusStringUtilsEditor")), TEXT("SiriusStringFormatArgument"));

		// Spawn a "Make Struct" node to create the struct needed for formatting the text.
		UK2Node_MakeStruct* MakeFormatArgumentDataStruct = CompilerContext.SpawnIntermediateNode<UK2Node_MakeStruct>(this, SourceGraph);
		MakeFormatArgumentDataStruct->StructType = FormatArgumentDataStruct;
		MakeFormatArgumentDataStruct->AllocateDefaultPins();
		MakeFormatArgumentDataStruct->bMadeAfterOverridePinRemoval = true;
		CompilerContext.MessageLog.NotifyIntermediateObjectCreation(MakeFormatArgumentDataStruct, this);

		// Set the struct's "ArgumentName" pin literal to be the argument pin's name.
		MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentName)), ArgumentPin->PinName.ToString());

		UEdGraphPin* ArgumentTypePin = MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValueType));

		// Move the connection of the argument pin to the correct argument value pin, and also set the correct argument type based on the pin that was hooked up.
		if (ArgumentPin->LinkedTo.Num() > 0)
		{
			const FName& ArgumentPinCategory = ArgumentPin->PinType.PinCategory;

			// Adds an implicit conversion node to this argument based on its function and pin name
			auto AddConversionNode = [&](const FName FuncName, const TCHAR* PinName)
			{
				// Set the default value if there was something passed in, or default to "String"
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("String"));

				// Spawn conversion node based on the given function name
				UK2Node_CallFunction* ToTextFunction = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
				ToTextFunction->SetFromFunction(UKismetStringLibrary::StaticClass()->FindFunctionByName(FuncName));
				ToTextFunction->AllocateDefaultPins();
				CompilerContext.MessageLog.NotifyIntermediateObjectCreation(ToTextFunction, this);

				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *ToTextFunction->FindPinChecked(PinName));

				ToTextFunction->FindPinChecked(TEXT("ReturnValue"))->MakeLinkTo(MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValue)));
			};
			
			if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Int)
			{
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("Int"));
				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValueInt)));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Int64)
			{
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("Int64"));
				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValueInt64)));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Float)
			{
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("Float"));
				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValueFloat)));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_String)
			{
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("String"));
				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValue)));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Byte && !ArgumentPin->PinType.PinSubCategoryObject.IsValid())
			{
				MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("Int"));

				// Need a manual cast from byte -> int
				UK2Node_CallFunction* CallByteToIntFunction = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
				CallByteToIntFunction->SetFromFunction(UKismetMathLibrary::StaticClass()->FindFunctionByName(GET_MEMBER_NAME_CHECKED(UKismetMathLibrary, Conv_ByteToInt)));
				CallByteToIntFunction->AllocateDefaultPins();
				CompilerContext.MessageLog.NotifyIntermediateObjectCreation(CallByteToIntFunction, this);

				// Move the byte output pin to the input pin of the conversion node
				CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *CallByteToIntFunction->FindPinChecked(TEXT("InByte")));

				// Connect the int output pin to the argument value
				CallByteToIntFunction->FindPinChecked(TEXT("ReturnValue"))->MakeLinkTo(MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValueInt)));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Boolean)
			{
				AddConversionNode(GET_MEMBER_NAME_CHECKED(UKismetStringLibrary, Conv_BoolToString), TEXT("InBool"));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Name)
			{
				AddConversionNode(GET_MEMBER_NAME_CHECKED(UKismetStringLibrary, Conv_NameToString), TEXT("InName"));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Text)
			{
				AddConversionNode(GET_MEMBER_NAME_CHECKED(UKismetTextLibrary, Conv_TextToString), TEXT("InText"));
			}
			else if (ArgumentPinCategory == UEdGraphSchema_K2::PC_Object)
			{
				AddConversionNode(GET_MEMBER_NAME_CHECKED(UKismetStringLibrary, Conv_ObjectToString), TEXT("InObj"));
			}
			else
			{
				// Unexpected pin type!
				CompilerContext.MessageLog.Error(*FText::Format(LOCTEXT("Error_UnexpectedPinType", "Pin '{0}' has an unexpected type: {1}"), FText::FromName(PinNames[ArgIdx]), FText::FromName(ArgumentPinCategory)).ToString());
			}
		}
		else
		{
			// No connected pin - just default to an empty string
			MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultValue(*ArgumentTypePin, TEXT("String"));
			MakeFormatArgumentDataStruct->GetSchema()->TrySetDefaultText(*MakeFormatArgumentDataStruct->FindPinChecked(GET_MEMBER_NAME_STRING_CHECKED(FSiriusStringFormatArgument, ArgumentValue)), FText::GetEmpty());
		}

		// The "Make Array" node already has one pin available, so don't create one for ArgIdx == 0
		if(ArgIdx > 0)
		{
			MakeArrayNode->AddInputPin();
		}

		// Find the input pin on the "Make Array" node by index.
		const FString PinName = FString::Printf(TEXT("[%d]"), ArgIdx);
		UEdGraphPin* InputPin = MakeArrayNode->FindPinChecked(PinName);

		// Find the output for the pin's "Make Struct" node and link it to the corresponding pin on the "Make Array" node.
		for (UEdGraphPin* Pin : MakeFormatArgumentDataStruct->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				Pin->MakeLinkTo(InputPin);
				break;
			}
		}
	}

	// Move connection of FormatText's "Result" pin to the call function's return value pin.
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(TEXT("Result")), *CallFormatFunction->GetReturnValuePin());
	// Move connection of FormatText's "Format" pin to the call function's "InPattern" pin
	CompilerContext.MovePinLinksToIntermediate(*GetFormatPin(), *CallFormatFunction->FindPinChecked(TEXT("InPattern")));

	BreakAllNodeLinks();
}

UK2Node::ERedirectType UK2Node_FormatString::DoPinsMatchForReconstruction(const UEdGraphPin* NewPin, int32 NewPinIndex, const UEdGraphPin* OldPin, int32 OldPinIndex) const
{
	ERedirectType RedirectType = ERedirectType_None;

	// if the pin names do match
	if (NewPin->PinName.ToString().Equals(OldPin->PinName.ToString(), ESearchCase::CaseSensitive))
	{
		// Make sure we're not dealing with a menu node
		UEdGraph* OuterGraph = GetGraph();
		if (OuterGraph && OuterGraph->Schema)
		{
			const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(GetSchema());
			if (!K2Schema || K2Schema->IsSelfPin(*NewPin) || K2Schema->ArePinTypesCompatible(OldPin->PinType, NewPin->PinType))
			{
				RedirectType = ERedirectType_Name;
			}
			else
			{
				RedirectType = ERedirectType_None;
			}
		}
	}
	else
	{
		// try looking for a redirect if it's a K2 node
		if (const UK2Node* Node = Cast<UK2Node>(NewPin->GetOwningNode()))
		{
			// if you don't have matching pin, now check if there is any redirect param set
			TArray<FString> OldPinNames;
			GetRedirectPinNames(*OldPin, OldPinNames);

			FName NewPinName;
			RedirectType = ShouldRedirectParam(OldPinNames, /*out*/ NewPinName, Node);

			// make sure they match
			if (RedirectType != ERedirectType_None && !NewPin->PinName.ToString().Equals(NewPinName.ToString(), ESearchCase::CaseSensitive))
			{
				RedirectType = ERedirectType_None;
			}
		}
	}

	return RedirectType;
}

bool UK2Node_FormatString::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	const UEdGraphPin* FormatPin = GetFormatPin();
	if (MyPin != FormatPin && MyPin->Direction == EGPD_Input)
	{
		const FName& OtherPinCategory = OtherPin->PinType.PinCategory;

		bool bIsValidType = false;
		if (OtherPinCategory == UEdGraphSchema_K2::PC_Int ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Int64 ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Float ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Text ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Byte && !OtherPin->PinType.PinSubCategoryObject.IsValid() ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Boolean ||
			OtherPinCategory == UEdGraphSchema_K2::PC_String ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Name ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Object ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			bIsValidType = true;
		}

		if (!bIsValidType)
		{
			OutReason = LOCTEXT("Error_InvalidArgumentType", "Format arguments may only be Byte, Integer, Float, Text, String, Name, Boolean, Object or Wildcard.").ToString();
			return true;
		}
	}

	return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
}

void UK2Node_FormatString::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	const UClass* ActionKey = GetClass();

	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);

		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_FormatString::GetMenuCategory() const
{
	return FEditorCategoryUtils::GetCommonCategory(FCommonEditorCategory::String);
}

UEdGraphPin* UK2Node_FormatString::GetFormatPin() const
{
	if (!CachedFormatPin)
	{
		const_cast<UK2Node_FormatString*>(this)->CachedFormatPin = FindPinChecked(TEXT("Format"));
	}
	return CachedFormatPin;
}

UEdGraphPin* UK2Node_FormatString::FindArgumentPin(const FName InPinName) const
{
	const UEdGraphPin* FormatPin = GetFormatPin();
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin != FormatPin && Pin->Direction != EGPD_Output && Pin->PinName.ToString().Equals(InPinName.ToString(), ESearchCase::CaseSensitive))
		{
			return Pin;
		}
	}

	return nullptr;
}

void UK2Node_FormatString::SynchronizeArgumentPinType(UEdGraphPin* Pin) const
{
	const UEdGraphPin* FormatPin = GetFormatPin();
	if (Pin != FormatPin && Pin->Direction == EGPD_Input)
	{
		bool bPinTypeChanged = false;
		if (Pin->LinkedTo.Num() == 0)
		{
			static const FEdGraphPinType WildcardPinType = FEdGraphPinType(UEdGraphSchema_K2::PC_Wildcard, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());

			// Ensure wildcard
			if (Pin->PinType != WildcardPinType)
			{
				Pin->PinType = WildcardPinType;
				bPinTypeChanged = true;
			}
		}
		else
		{
			const UEdGraphPin* ArgumentSourcePin = Pin->LinkedTo[0];

			// Take the type of the connected pin
			if (Pin->PinType != ArgumentSourcePin->PinType)
			{
				Pin->PinType = ArgumentSourcePin->PinType;
				bPinTypeChanged = true;
			}
		}

		if (bPinTypeChanged)
		{
			// Let the graph know to refresh
			GetGraph()->NotifyGraphChanged();

			UBlueprint* Blueprint = GetBlueprint();
			if (!Blueprint->bBeingCompiled)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
				Blueprint->BroadcastChanged();
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
