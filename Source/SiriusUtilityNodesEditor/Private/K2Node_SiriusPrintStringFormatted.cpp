// Copyright 2022-2022 Jasper de Laat. All Rights Reserved.

#include "K2Node_SiriusPrintStringFormatted.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EditorCategoryUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_SiriusFormatString.h"
#include "KismetCompiler.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"

#define LOCTEXT_NAMESPACE "K2Node_SiriusPrintStringFormatted"

const FName UK2Node_SiriusPrintStringFormatted::ExecutePinName = UEdGraphSchema_K2::PN_Execute;
const FName UK2Node_SiriusPrintStringFormatted::ThenPinName = UEdGraphSchema_K2::PN_Then;
const FName UK2Node_SiriusPrintStringFormatted::FormatPinName = TEXT("In String");
const FName UK2Node_SiriusPrintStringFormatted::PrintScreenPinName = TEXT("Print to Screen");
const FName UK2Node_SiriusPrintStringFormatted::PrintLogPinName = TEXT("Print to Log");
const FName UK2Node_SiriusPrintStringFormatted::TextColorPinName = TEXT("Text Color");
const FName UK2Node_SiriusPrintStringFormatted::DurationPinName = TEXT("Duration");

UK2Node_SiriusPrintStringFormatted::UK2Node_SiriusPrintStringFormatted()
{
	NodeTooltip = LOCTEXT("NodeTooltip", "Prints a formatted string to the log, and optionally, to the screen.\n If Print To Log is true, it will be visible in the Output Log window. Otherwise it will be logged only as 'Verbose', so it generally won't show up.");

	// Show the development only banner to warn the user they're not going to get the benefits of this node in a shipping build
	SetEnabledState(ENodeEnabledState::DevelopmentOnly, false);
}

void UK2Node_SiriusPrintStringFormatted::AllocateDefaultPins()
{
	Super::AllocateDefaultPins();

	// AdvancedPinDisplay is serialized. Any other value than NoPins might be from user input, don't overwrite those.
	if (AdvancedPinDisplay == ENodeAdvancedPins::NoPins)
	{
		AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
	}

	const UEdGraphSchema_K2* DefaultSchema = GetDefault<UEdGraphSchema_K2>();

	// Execution pins
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, ExecutePinName);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, ThenPinName);

	// Format pins
	UEdGraphPin* FormatPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, FormatPinName);
	DefaultSchema->SetPinAutogeneratedDefaultValue(FormatPin, TEXT("Hello"));
	for (const FName& PinName : PinNames)
	{
		CachedArgumentPins.Emplace(CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, PinName));
	}

	UEdGraphPin* PrintScreenPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Boolean, PrintScreenPinName);
	PrintScreenPin->bAdvancedView = true;
	DefaultSchema->SetPinAutogeneratedDefaultValue(PrintScreenPin, TEXT("true"));

	UEdGraphPin* PrintLogPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Boolean, PrintLogPinName);
	PrintLogPin->bAdvancedView = true;
	DefaultSchema->SetPinAutogeneratedDefaultValue(PrintLogPin, TEXT("true"));

	UScriptStruct* LinearColorScriptStruct = FindObjectChecked<UScriptStruct>(ANY_PACKAGE, TEXT("LinearColor"));
	UEdGraphPin* TextColorPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Struct, LinearColorScriptStruct, TextColorPinName);
	TextColorPin->bAdvancedView = true;
	DefaultSchema->SetPinAutogeneratedDefaultValue(TextColorPin, FLinearColor(0.0f, 0.66f, 1.0f).ToString());

	UEdGraphPin* DurationPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Real, UEdGraphSchema_K2::PC_Float, DurationPinName);
	DurationPin->bAdvancedView = true;
	DefaultSchema->SetPinAutogeneratedDefaultValue(DurationPin, TEXT("2.0"));
}

FText UK2Node_SiriusPrintStringFormatted::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("NodeTitle", "Print String Formatted (Sirius)");
}

FText UK2Node_SiriusPrintStringFormatted::GetPinDisplayName(const UEdGraphPin* Pin) const
{
	// Don't show the names of the execution pins.
	if (Pin != GetExecutePin() && Pin != GetThenPin())
	{
		return FText::FromName(Pin->PinName);
	}

	return FText::GetEmpty();
}

FText UK2Node_SiriusPrintStringFormatted::GetTooltipText() const
{
	return NodeTooltip;
}

void UK2Node_SiriusPrintStringFormatted::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Modify();

	// Potentially update an argument pin type
	SynchronizeArgumentPinType(Pin);
}

void UK2Node_SiriusPrintStringFormatted::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	// Detect if the format pin has changed.
	const UEdGraphPin* FormatPin = GetFormatPin();
	if (Pin == FormatPin && FormatPin->LinkedTo.Num() == 0)
	{
		TArray<FString> ArgumentParams;
		FTextFormat::FromString(FormatPin->DefaultValue).GetFormatArgumentNames(ArgumentParams);

		PinNames.Reset();

		// Create argument pins if new arguments were created.
		for (const FString& Param : ArgumentParams)
		{
			const FName ParamName(*Param);
			if (!FindArgumentPin(ParamName))
			{
				// Insert the newly created argument pin(s) after the format pin and before the advanced option pins.
				FCreatePinParams CreatePinParams;
				CreatePinParams.Index = Pins.IndexOfByKey(FormatPin) + 1 + CachedArgumentPins.Num();
				CachedArgumentPins.Emplace(CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, ParamName, CreatePinParams));
			}
			PinNames.Add(ParamName);
		}

		// Destroy argument pins whose arguments were destroyed.
		for (auto It = Pins.CreateIterator(); It; ++It)
		{
			UEdGraphPin* CheckPin = *It;
			if (FindArgumentPin(CheckPin->PinName))
			{
				const bool bIsValidArgPin = ArgumentParams.ContainsByPredicate([&CheckPin](const FString& InPinName)
				{
					return InPinName.Equals(CheckPin->PinName.ToString(), ESearchCase::CaseSensitive);
				});

				if (!bIsValidArgPin)
				{
					CheckPin->MarkAsGarbage();
					It.RemoveCurrent();
					CachedArgumentPins.Remove(CheckPin);
				}
			}
		}

		// Notify graph that something changed.
		GetGraph()->NotifyGraphChanged();
	}
}

void UK2Node_SiriusPrintStringFormatted::PinTypeChanged(UEdGraphPin* Pin)
{
	// Potentially update an argument pin type
	SynchronizeArgumentPinType(Pin);

	Super::PinTypeChanged(Pin);
}

void UK2Node_SiriusPrintStringFormatted::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	// Create a "FormatString" node to do the heavy lifting regarding the format string.
	UK2Node_SiriusFormatString* FormatStringNode = CompilerContext.SpawnIntermediateNode<UK2Node_SiriusFormatString>(this, SourceGraph);
	FormatStringNode->AllocateDefaultPins();
	CompilerContext.MessageLog.NotifyIntermediateObjectCreation(FormatStringNode, this);

	// Move the format and argument pins to the format string node.
	CompilerContext.MovePinLinksToIntermediate(*GetFormatPin(), *FormatStringNode->GetFormatPin());
	for (int32 ArgIdx = 0; ArgIdx < PinNames.Num(); ++ArgIdx)
	{
		const FName& PinName = PinNames[ArgIdx];
		UEdGraphPin* ArgumentPin = FindArgumentPin(PinName);
		UEdGraphPin* TargetPin = FormatStringNode->AddArgumentPin(PinName);
		CompilerContext.MovePinLinksToIntermediate(*ArgumentPin, *TargetPin);
		FormatStringNode->SynchronizeArgumentPinType(TargetPin);
	}

	// Create a "PrintString" function node.
	UK2Node_CallFunction* PrintStringNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	const UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
	PrintStringNode->SetFromFunction(Function);
	PrintStringNode->AllocateDefaultPins();
	CompilerContext.MessageLog.NotifyIntermediateObjectCreation(PrintStringNode, this);
	
	// Link pins with print string function node.
	CompilerContext.MovePinLinksToIntermediate(*GetExecPin(), *PrintStringNode->GetExecPin());
	FormatStringNode->GetResultPin()->MakeLinkTo(PrintStringNode->FindPinChecked(TEXT("InString")));
	CompilerContext.MovePinLinksToIntermediate(*GetPrintScreenPin(), *PrintStringNode->FindPinChecked(TEXT("bPrintToScreen")));
	CompilerContext.MovePinLinksToIntermediate(*GetPrintLogPin(), *PrintStringNode->FindPinChecked(TEXT("bPrintToLog")));
	CompilerContext.MovePinLinksToIntermediate(*GetTextColorPin(), *PrintStringNode->FindPinChecked(TEXT("TextColor")));
	CompilerContext.MovePinLinksToIntermediate(*GetDurationPin(), *PrintStringNode->FindPinChecked(TEXT("Duration")));
	CompilerContext.MovePinLinksToIntermediate(*GetThenPin(), *PrintStringNode->GetThenPin());

	// Final step, break all links to this node as we've finished expanding it.
	BreakAllNodeLinks();
}

void UK2Node_SiriusPrintStringFormatted::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	const UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_SiriusPrintStringFormatted::GetMenuCategory() const
{
	return FEditorCategoryUtils::GetCommonCategory(FCommonEditorCategory::String);
}

bool UK2Node_SiriusPrintStringFormatted::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	if (FindArgumentPin(MyPin->PinName))
	{
		const FName& OtherPinCategory = OtherPin->PinType.PinCategory;

		bool bIsValidType = false;
		if (OtherPinCategory == UEdGraphSchema_K2::PC_Int ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Int64 ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Real ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Text ||
			OtherPinCategory == UEdGraphSchema_K2::PC_Byte ||
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
			OutReason = LOCTEXT("Error_InvalidArgumentType", "Format arguments may only be Byte, Enum, Integer, Float, Text, String, Name, Boolean, Object or Wildcard.").ToString();
			return true;
		}
	}

	return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
}

void UK2Node_SiriusPrintStringFormatted::PostReconstructNode()
{
	Super::PostReconstructNode();

	if (!IsTemplate())
	{
		// Make sure we're not dealing with a menu node
		if (GetSchema())
		{
			for (UEdGraphPin* CurrentPin : Pins)
			{
				// Potentially update an argument pin type
				SynchronizeArgumentPinType(CurrentPin);
			}
		}
	}
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetExecutePin() const
{
	return FindPinChecked(ExecutePinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetThenPin() const
{
	return FindPinChecked(ThenPinName, EGPD_Output);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetFormatPin() const
{
	return FindPinChecked(FormatPinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetPrintScreenPin() const
{
	return FindPinChecked(PrintScreenPinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetPrintLogPin() const
{
	return FindPinChecked(PrintLogPinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetTextColorPin() const
{
	return FindPinChecked(TextColorPinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::GetDurationPin() const
{
	return FindPinChecked(DurationPinName, EGPD_Input);
}

UEdGraphPin* UK2Node_SiriusPrintStringFormatted::FindArgumentPin(const FName PinName) const
{
	// Check if cache is out-of-date.
	if (CachedArgumentPins.Num() != PinNames.Num())
	{
		const_cast<UK2Node_SiriusPrintStringFormatted*>(this)->CachedArgumentPins.Reset();

		const TArray<UEdGraphPin*> IgnorePins = {GetExecutePin(), GetThenPin(), GetFormatPin(), GetPrintScreenPin(), GetPrintLogPin(), GetTextColorPin(), GetDurationPin()};
		for (UEdGraphPin* const Pin : Pins)
		{
			if (!IgnorePins.Contains(Pin))
			{
				const_cast<UK2Node_SiriusPrintStringFormatted*>(this)->CachedArgumentPins.Emplace(Pin);
			}
		}
	}

	UEdGraphPin* const* Find = CachedArgumentPins.FindByPredicate([&](const UEdGraphPin* const ArgumentPin)
	{
		return ArgumentPin->PinName.ToString().Equals(PinName.ToString(), ESearchCase::CaseSensitive);
	});
	return Find ? *Find : nullptr;
}

void UK2Node_SiriusPrintStringFormatted::SynchronizeArgumentPinType(UEdGraphPin* Pin) const
{
	if (FindArgumentPin(Pin->PinName))
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
